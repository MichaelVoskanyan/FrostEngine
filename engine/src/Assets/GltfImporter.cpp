#include <Frost/Assets/GltfImporter.hpp>

#include <Frost/Assets/Image.hpp>
#include <Frost/Assets/ImportError.hpp>
#include <Frost/Units.hpp>

#include "Json.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Byte-order note: glTF binary data is little-endian and is read here with memcpy into host
// integers/floats, i.e. the importer assumes a little-endian host (as does the rest of the engine,
// e.g. SPIR-V loading).

namespace {

    using Frost::ImageData;
    using Frost::ImageImporter;
    using Frost::ImportError;
    using Frost::Json::Value;
    using Frost::MaterialData;
    using Frost::MaterialParameters;
    using Frost::MeshData;
    using Frost::SubMesh;
    using Frost::TextureData;
    using Frost::TextureFilter;
    using Frost::TextureMipmapMode;
    using Frost::TextureSettings;
    using Frost::TextureWrap;
    using Frost::Vertex;

    // GLB container constants (glTF 2.0 specification, section 4: "GLB File Format").
    constexpr std::uint32_t kGlbMagic = 0x46546C67;      // "glTF"
    constexpr std::uint32_t kGlbVersion = 2;
    constexpr std::uint32_t kGlbChunkJson = 0x4E4F534A;  // "JSON"
    constexpr std::uint32_t kGlbChunkBin = 0x004E4942;   // "BIN\0"
    constexpr std::size_t kGlbHeaderSize = 12;           // magic + version + length
    constexpr std::size_t kGlbChunkHeaderSize = 8;       // chunkLength + chunkType

    // accessor.componentType
    enum class ComponentType : int {
        Byte = 5120,
        UnsignedByte = 5121,
        Short = 5122,
        UnsignedShort = 5123,
        UnsignedInt = 5125,
        Float = 5126,
    };

    // primitive.mode
    enum class PrimitiveMode : int {
        Points = 0,
        Lines = 1,
        LineLoop = 2,
        LineStrip = 3,
        Triangles = 4,
        TriangleStrip = 5,
        TriangleFan = 6,
    };

    // Extensions that change how geometry is encoded. A file that *requires* one of them cannot be
    // decoded by this importer; other required extensions (materials, texture transforms, ...)
    // do not affect the geometry and are ignored along with the data they describe.
    constexpr std::array<std::string_view, 2> kUnsupportedRequiredExtensions = {
            "KHR_draco_mesh_compression",
            "EXT_meshopt_compression",
    };

    template <typename... Parts>
    [[nodiscard]]
    ImportError Error(const Parts&... parts) {
        std::ostringstream message;
        (message << ... << parts);
        return ImportError(message.str());
    }

    [[nodiscard]]
    std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if (!file.is_open()) { throw Error("Failed to open file '", path.string(), "'."); }

        const std::streamoff size = file.tellg();
        if (size < 0) { throw Error("Failed to determine the size of file '", path.string(), "'."); }

        std::vector<std::byte> bytes(static_cast<std::size_t>(size));

        file.seekg(0);
        if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            throw Error("Failed to read file '", path.string(), "'.");
        }

        return bytes;
    }

    // ---------------------------------------------------------------------------------------------
    // File container: a GLB (JSON chunk + optional BIN chunk) or a plain JSON .gltf
    // ---------------------------------------------------------------------------------------------

    struct GltfContents {
        std::string_view json;           // The JSON document; a view into the file bytes.
        std::span<const std::byte> bin;  // The GLB BIN chunk (empty for .gltf, or a GLB without one).
    };

    [[nodiscard]]
    std::uint32_t ReadUint32(const std::span<const std::byte> bytes, const std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]]
    GltfContents ParseContainer(const std::span<const std::byte> bytes) {
        // Anything that does not start with the GLB magic is taken to be a JSON .gltf document
        // (whose external buffers/images are fetched by URI as they are needed). Skip a UTF-8 BOM
        // and leading whitespace to check that it at least looks like one.
        if (bytes.size() < kGlbHeaderSize || ReadUint32(bytes, 0) != kGlbMagic) {
            std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (text.starts_with("\xEF\xBB\xBF")) { text.remove_prefix(3); }
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                                     text.front() == '\r')) {
                text.remove_prefix(1);
            }

            if (!text.starts_with('{')) {
                throw Error("Not a glTF file: neither a GLB container (bad magic number) nor a JSON document.");
            }

            return GltfContents{.json = text, .bin = {}};
        }

        const std::uint32_t version = ReadUint32(bytes, 4);
        if (version != kGlbVersion) { throw Error("Unsupported GLB container version ", version, " (expected 2)."); }

        const std::uint32_t length = ReadUint32(bytes, 8);
        if (length > bytes.size()) {
            throw Error("GLB header declares ", length, " bytes but the file holds only ", bytes.size(),
                        " (truncated file?).");
        }
        if (length < kGlbHeaderSize) { throw Error("GLB header declares an impossible total length."); }

        GltfContents contents;
        bool haveJson = false;
        bool haveBin = false;

        std::size_t offset = kGlbHeaderSize;
        while (offset + kGlbChunkHeaderSize <= length) {
            const std::uint32_t chunkLength = ReadUint32(bytes, offset);
            const std::uint32_t chunkType = ReadUint32(bytes, offset + 4);
            const std::size_t dataOffset = offset + kGlbChunkHeaderSize;

            if (chunkLength > length - dataOffset) { throw Error("GLB chunk extends past the end of the file."); }

            if (!haveJson) {
                // The first chunk must be the JSON chunk.
                if (chunkType != kGlbChunkJson) { throw Error("First GLB chunk is not the JSON chunk."); }
                contents.json = std::string_view(reinterpret_cast<const char*>(bytes.data()) + dataOffset, chunkLength);
                haveJson = true;
            }
            else if (!haveBin && chunkType == kGlbChunkBin) {
                contents.bin = bytes.subspan(dataOffset, chunkLength);
                haveBin = true;
            }
            // Chunks of unknown type are ignored, as the specification requires.

            offset = dataOffset + chunkLength;
        }

        if (!haveJson) { throw Error("GLB container has no JSON chunk."); }

        return contents;
    }

    // ---------------------------------------------------------------------------------------------
    // JSON field helpers
    // ---------------------------------------------------------------------------------------------

    // Integer members (indices, counts, offsets) are all non-negative in glTF. Fractional, negative
    // or absurdly large values mean a corrupt file, not a value to round.
    [[nodiscard]]
    std::int64_t AsNonNegativeInteger(const Value& value, const std::string_view key) {
        if (!value.IsNumber()) { throw Error("Property '", key, "' is not a number."); }

        const double number = value.AsNumber();
        constexpr double kMaxExact = 9007199254740992.0;  // 2^53

        if (number < 0.0 || number > kMaxExact || std::floor(number) != number) {
            throw Error("Property '", key, "' is not a valid non-negative integer (", number, ").");
        }

        return static_cast<std::int64_t>(number);
    }

    // Returns `fallback` if `key` is absent.
    [[nodiscard]]
    std::int64_t GetInteger(const Value& object, const std::string_view key, const std::int64_t fallback) {
        const Value* const member = object.Find(key);
        return member == nullptr ? fallback : AsNonNegativeInteger(*member, key);
    }

    [[nodiscard]]
    std::int64_t RequireInteger(const Value& object, const std::string_view key) {
        const Value* const member = object.Find(key);
        if (member == nullptr) { throw Error("Required property '", key, "' is missing."); }
        return AsNonNegativeInteger(*member, key);
    }

    // Returns `fallback` if `key` is absent.
    [[nodiscard]]
    float GetNumber(const Value& object, const std::string_view key, const float fallback) {
        const Value* const member = object.Find(key);
        if (member == nullptr) { return fallback; }
        if (!member->IsNumber()) { throw Error("Property '", key, "' is not a number."); }
        return static_cast<float>(member->AsNumber());
    }

    [[nodiscard]]
    bool GetBoolean(const Value& object, const std::string_view key, const bool fallback) {
        const Value* const member = object.Find(key);
        if (member == nullptr) { return fallback; }
        if (!member->IsBoolean()) { throw Error("Property '", key, "' is not a boolean."); }
        return member->AsBoolean();
    }

    // Reads a fixed-size numeric array member into `out`. Returns false if the member is absent.
    [[nodiscard]]
    bool GetNumberArray(const Value& object, const std::string_view key, const std::span<float> out) {
        const Value* const member = object.Find(key);
        if (member == nullptr) { return false; }

        if (!member->IsArray() || member->AsArray().size() != out.size()) {
            throw Error("Property '", key, "' is not an array of ", out.size(), " numbers.");
        }

        for (std::size_t i = 0; i < out.size(); ++i) {
            const Value& element = member->AsArray()[i];
            if (!element.IsNumber()) { throw Error("Property '", key, "' contains a non-numeric element."); }
            out[i] = static_cast<float>(element.AsNumber());
        }

        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // Geometry helpers
    // ---------------------------------------------------------------------------------------------

    [[nodiscard]]
    glm::vec3 SafeNormalize(const glm::vec3& v) noexcept {
        const float lengthSquared = glm::dot(v, v);
        return lengthSquared > 0.0f ? v / std::sqrt(lengthSquared) : v;
    }

    // Derives per-vertex tangents (xyz, plus bitangent handedness in w) from the triangles' UV
    // layout: each triangle's tangent is the object-space direction along which u increases,
    // accumulated into its vertices, then made perpendicular to the vertex normal. Triangles with
    // degenerate UVs or positions contribute nothing; vertices that end up without a usable
    // tangent get an arbitrary one perpendicular to their normal, so normal mapping stays defined.
    void GenerateTangents(std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
        std::vector<glm::vec3> tangentSums(vertices.size(), glm::vec3(0.0f));
        std::vector<glm::vec3> bitangentSums(vertices.size(), glm::vec3(0.0f));

        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const Vertex& a = vertices[indices[i + 0]];
            const Vertex& b = vertices[indices[i + 1]];
            const Vertex& c = vertices[indices[i + 2]];

            const glm::vec3 edge1 = b.position - a.position;
            const glm::vec3 edge2 = c.position - a.position;
            const glm::vec2 deltaUv1 = b.uv - a.uv;
            const glm::vec2 deltaUv2 = c.uv - a.uv;

            const float area = deltaUv1.x * deltaUv2.y - deltaUv1.y * deltaUv2.x;
            if (area == 0.0f) { continue; }  // UVs collapsed to a line/point: no direction to derive.

            const float r = 1.0f / area;
            const glm::vec3 tangent = (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * r;
            const glm::vec3 bitangent = (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * r;

            for (std::size_t corner = 0; corner < 3; ++corner) {
                tangentSums[indices[i + corner]] += tangent;
                bitangentSums[indices[i + corner]] += bitangent;
            }
        }

        for (std::size_t i = 0; i < vertices.size(); ++i) {
            Vertex& vertex = vertices[i];
            const glm::vec3 normal = vertex.normal;

            // Gram-Schmidt: remove the normal component, then normalize.
            glm::vec3 tangent = SafeNormalize(tangentSums[i] - normal * glm::dot(normal, tangentSums[i]));

            if (glm::dot(tangent, tangent) == 0.0f) {
                // No UV-derived tangent: any direction perpendicular to the normal will do (the
                // normal map's orientation is then arbitrary, but the math stays finite).
                const glm::vec3 axis =
                        std::abs(normal.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
                tangent = SafeNormalize(glm::cross(normal, axis));
            }

            // Handedness: +1 if the UV-derived bitangent agrees with cross(N, T), -1 if mirrored.
            const float handedness = glm::dot(glm::cross(normal, tangent), bitangentSums[i]) < 0.0f ? -1.0f : 1.0f;

            vertex.tangent = glm::vec4(tangent, handedness);
        }
    }

    // Expands TRIANGLE_STRIP / TRIANGLE_FAN index sequences into a plain triangle list, using the
    // specification's vertex orderings so every triangle keeps the strip's/fan's winding. A
    // TRIANGLES list is returned with any trailing partial triangle dropped (which is also what
    // the GPU would do with it).
    [[nodiscard]]
    std::vector<std::uint32_t> ToTriangleList(const std::vector<std::uint32_t>& indices, const PrimitiveMode mode) {
        std::vector<std::uint32_t> triangles;

        switch (mode) {
            case PrimitiveMode::Triangles:
                triangles.assign(indices.begin(),
                                 indices.begin() + static_cast<std::ptrdiff_t>(indices.size() / 3 * 3));
                break;

            case PrimitiveMode::TriangleStrip:
                if (indices.size() < 3) { break; }
                triangles.reserve((indices.size() - 2) * 3);
                for (std::size_t i = 0; i + 2 < indices.size(); ++i) {
                    // Spec: (p_i, p_{i+(1+i%2)}, p_{i+(2-i%2)}).
                    triangles.push_back(indices[i]);
                    triangles.push_back(indices[i + 1 + (i % 2)]);
                    triangles.push_back(indices[i + 2 - (i % 2)]);
                }
                break;

            case PrimitiveMode::TriangleFan:
                if (indices.size() < 3) { break; }
                triangles.reserve((indices.size() - 2) * 3);
                for (std::size_t i = 0; i + 2 < indices.size(); ++i) {
                    // Spec: (p_{i+1}, p_{i+2}, p_0).
                    triangles.push_back(indices[i + 1]);
                    triangles.push_back(indices[i + 2]);
                    triangles.push_back(indices[0]);
                }
                break;

            default: break;  // Points/lines: not triangle geometry; the caller has filtered these out.
        }

        return triangles;
    }

    // ---------------------------------------------------------------------------------------------
    // URI helpers (images may live in the BIN chunk, in a base64 data: URI, or in a file next to
    // the model)
    // ---------------------------------------------------------------------------------------------

    [[nodiscard]]
    std::vector<std::byte> DecodeBase64(const std::string_view text) {
        auto valueOf = [](const char c) -> int {
            if (c >= 'A' && c <= 'Z') { return c - 'A'; }
            if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
            if (c >= '0' && c <= '9') { return c - '0' + 52; }
            if (c == '+') { return 62; }
            if (c == '/') { return 63; }
            return -1;
        };

        std::vector<std::byte> out;
        out.reserve(text.size() / 4 * 3);

        std::uint32_t accumulator = 0;
        int bits = 0;

        for (const char c : text) {
            if (c == '=') { break; }  // Padding: the remaining bits (if any) are discarded below.
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') { continue; }

            const int value = valueOf(c);
            if (value < 0) { throw Error("Invalid character in base64 data URI."); }

            accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
            bits += 6;

            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<std::byte>((accumulator >> bits) & 0xFF));
            }
        }

        return out;
    }

    // Decodes %XX escapes of a relative URI so it can be used as a file name.
    [[nodiscard]]
    std::string PercentDecode(const std::string_view uri) {
        auto hexValue = [](const char c) -> int {
            if (c >= '0' && c <= '9') { return c - '0'; }
            if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
            if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
            return -1;
        };

        std::string out;
        out.reserve(uri.size());

        for (std::size_t i = 0; i < uri.size(); ++i) {
            if (uri[i] == '%' && i + 2 < uri.size() && hexValue(uri[i + 1]) >= 0 && hexValue(uri[i + 2]) >= 0) {
                out.push_back(static_cast<char>(hexValue(uri[i + 1]) * 16 + hexValue(uri[i + 2])));
                i += 2;
            }
            else {
                out.push_back(uri[i]);
            }
        }

        return out;
    }

    // Fetches the bytes a glTF `uri` refers to: an embedded "data:" URI or a file path relative to
    // the model. Absolute paths and remote URLs are refused.
    [[nodiscard]]
    std::vector<std::byte> ResolveUri(const std::string_view uri, const std::filesystem::path& baseDirectory) {
        if (uri.starts_with("data:")) {
            const std::size_t comma = uri.find(',');
            if (comma == std::string_view::npos) { throw Error("Malformed data: URI."); }

            const std::string_view header = uri.substr(0, comma);
            if (!header.ends_with(";base64")) { throw Error("Only base64 data: URIs are supported."); }

            return DecodeBase64(uri.substr(comma + 1));
        }

        if (uri.find("://") != std::string_view::npos) {
            throw Error("Remote URI '", uri, "' is not supported.");
        }

        const std::string decoded = PercentDecode(uri);
        const std::filesystem::path relative(
                std::u8string(reinterpret_cast<const char8_t*>(decoded.data()), decoded.size()));
        if (relative.is_absolute()) { throw Error("Absolute URI '", uri, "' is not supported."); }

        return ReadFile(baseDirectory / relative);
    }

    // ---------------------------------------------------------------------------------------------
    // Document: the parsed JSON plus the BIN payload, with accessor decoding, material/texture
    // resolution and scene traversal.
    // ---------------------------------------------------------------------------------------------

    class Document final {
    public:
        // `baseDirectory` is where relative buffer/image URIs are resolved (the model's own directory).
        Document(const Value& root, const std::span<const std::byte> bin, std::filesystem::path baseDirectory)
            : m_Root(root), m_Bin(bin), m_BaseDirectory(std::move(baseDirectory)) {}

        void CheckCompatibility() const {
            if (const Value* const asset = m_Root.Find("asset")) {
                if (const Value* const version = asset->Find("version")) {
                    if (!version->IsString() || !version->AsString().starts_with("2.")) {
                        throw Error("Unsupported glTF asset version '",
                                    version->IsString() ? version->AsString() : std::string("?"), "' (expected 2.x).");
                    }
                }
            }

            const Value* const required = m_Root.Find("extensionsRequired");
            if (required == nullptr || !required->IsArray()) { return; }

            for (const Value& extension : required->AsArray()) {
                if (!extension.IsString()) { continue; }
                if (std::find(kUnsupportedRequiredExtensions.begin(), kUnsupportedRequiredExtensions.end(),
                              extension.AsString()) != kUnsupportedRequiredExtensions.end()) {
                    throw Error("File requires the '", extension.AsString(), "' extension, which is not supported.");
                }
            }
        }

        // Walks the scene graph and appends every triangle primitive reached to `out`, with each
        // node's world transform baked into the vertices.
        void ImportGeometry(MeshData& out) {
            struct PendingNode {
                std::int64_t index;
                glm::mat4 parentWorld;
            };

            const std::size_t nodeCount = ArraySize("nodes");

            // glTF is in meters, the world in centimeters (Frost/Units.hpp): the whole scene is
            // scaled up by that, above its root nodes, so node translations are converted along
            // with the geometry. (A uniform scale leaves normals and tangents as they are.)
            const glm::mat4 metersToWorld = glm::scale(glm::mat4(1.0f), glm::vec3(Frost::kMeter));

            // Roots (and, below, children) are pushed in reverse so the stack pops them in file
            // order. Purely cosmetic: it keeps the sub-mesh order predictable.
            const std::vector<std::int64_t> roots = GetRootNodes();
            std::vector<PendingNode> stack;
            for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
                stack.push_back(PendingNode{.index = *it, .parentWorld = metersToWorld});
            }

            // glTF requires the node hierarchy to be a tree (no node has two parents, no cycles).
            // Tracking visits turns a violating file into a clear error instead of an endless loop.
            std::vector<bool> visited(nodeCount, false);

            while (!stack.empty()) {
                const PendingNode pending = stack.back();
                stack.pop_back();

                if (visited[static_cast<std::size_t>(pending.index)]) {
                    throw Error("Node ", pending.index,
                                " is reachable more than once; the node hierarchy is not a tree.");
                }
                visited[static_cast<std::size_t>(pending.index)] = true;

                const Value& node = Lookup("nodes", pending.index);
                const glm::mat4 world = pending.parentWorld * GetLocalTransform(node, pending.index);

                if (const Value* const mesh = node.Find("mesh")) {
                    AppendMesh(AsNonNegativeInteger(*mesh, "mesh"), GetName(node), world, out);
                }

                if (const Value* const children = node.Find("children")) {
                    if (!children->IsArray()) { throw Error("Node ", pending.index, " has a non-array 'children'."); }

                    const auto& childArray = children->AsArray();
                    for (auto it = childArray.rbegin(); it != childArray.rend(); ++it) {
                        const std::int64_t child = AsNonNegativeInteger(*it, "children");
                        if (static_cast<std::size_t>(child) >= nodeCount) {
                            throw Error("Node ", pending.index, " references non-existent child node ", child, ".");
                        }
                        stack.push_back(PendingNode{.index = child, .parentWorld = world});
                    }
                }
            }
        }

    private:
        // A resolved accessor: element layout plus a pointer into the BIN chunk, fully bounds-checked.
        struct Accessor {
            ComponentType componentType = ComponentType::Float;
            std::size_t componentSize = 4;   // Bytes per component.
            std::size_t componentCount = 1;  // Components per element (SCALAR 1 ... VEC4 4).
            std::size_t count = 0;           // Elements.
            std::size_t stride = 0;          // Bytes between consecutive elements.
            bool normalized = false;
            const std::byte* data = nullptr;  // Null for accessors without a bufferView (all zeros).
        };

        [[nodiscard]]
        std::size_t ArraySize(const std::string_view arrayName) const {
            const Value* const array = m_Root.Find(arrayName);
            if (array == nullptr) { return 0; }
            if (!array->IsArray()) { throw Error("Top-level '", arrayName, "' is not an array."); }
            return array->AsArray().size();
        }

        // root[arrayName][index], which must exist and be an object.
        [[nodiscard]]
        const Value& Lookup(const std::string_view arrayName, const std::int64_t index) const {
            const Value* const array = m_Root.Find(arrayName);
            const Value* const element =
                    array == nullptr ? nullptr : array->At(static_cast<std::size_t>(index));

            if (element == nullptr) { throw Error("Reference to non-existent ", arrayName, "[", index, "]."); }
            if (!element->IsObject()) { throw Error(arrayName, "[", index, "] is not an object."); }

            return *element;
        }

        // The default scene's root nodes; or the first scene's if no default is set; or, for a
        // file without scenes, every node that is not some other node's child.
        [[nodiscard]]
        std::vector<std::int64_t> GetRootNodes() const {
            const std::size_t nodeCount = ArraySize("nodes");
            std::vector<std::int64_t> roots;

            if (ArraySize("scenes") > 0) {
                const Value& scene = Lookup("scenes", GetInteger(m_Root, "scene", 0));

                if (const Value* const nodes = scene.Find("nodes")) {
                    if (!nodes->IsArray()) { throw Error("Scene 'nodes' is not an array."); }
                    for (const Value& node : nodes->AsArray()) {
                        roots.push_back(AsNonNegativeInteger(node, "nodes"));
                    }
                }
            }
            else {
                std::vector<bool> isChild(nodeCount, false);

                for (std::size_t i = 0; i < nodeCount; ++i) {
                    const Value* const children = Lookup("nodes", static_cast<std::int64_t>(i)).Find("children");
                    if (children == nullptr || !children->IsArray()) { continue; }
                    for (const Value& child : children->AsArray()) {
                        const std::int64_t childIndex = AsNonNegativeInteger(child, "children");
                        if (static_cast<std::size_t>(childIndex) < nodeCount) {
                            isChild[static_cast<std::size_t>(childIndex)] = true;
                        }
                    }
                }

                for (std::size_t i = 0; i < nodeCount; ++i) {
                    if (!isChild[i]) { roots.push_back(static_cast<std::int64_t>(i)); }
                }
            }

            for (const std::int64_t root : roots) {
                if (static_cast<std::size_t>(root) >= nodeCount) {
                    throw Error("Scene references non-existent node ", root, ".");
                }
            }

            return roots;
        }

        // A node's local transform: either its explicit column-major `matrix`, or the composition
        // of its `translation`, `rotation` (unit quaternion, xyzw) and `scale` properties (T*R*S).
        [[nodiscard]]
        static glm::mat4 GetLocalTransform(const Value& node, const std::int64_t nodeIndex) {
            std::array<float, 16> matrix{};
            if (GetNumberArray(node, "matrix", matrix)) {
                if (node.Find("translation") != nullptr || node.Find("rotation") != nullptr ||
                    node.Find("scale") != nullptr) {
                    throw Error("Node ", nodeIndex, " specifies both 'matrix' and translation/rotation/scale.");
                }
                return glm::make_mat4(matrix.data());  // glTF and glm are both column-major.
            }

            std::array<float, 3> translation = {0.0f, 0.0f, 0.0f};
            std::array<float, 4> rotation = {0.0f, 0.0f, 0.0f, 1.0f};
            std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};

            static_cast<void>(GetNumberArray(node, "translation", translation));
            static_cast<void>(GetNumberArray(node, "rotation", rotation));
            static_cast<void>(GetNumberArray(node, "scale", scale));

            // Assigned member-wise: glm::quat's constructor argument order depends on a
            // configuration macro (wxyz by default), while glTF is always xyzw.
            glm::quat quaternion;
            quaternion.x = rotation[0];
            quaternion.y = rotation[1];
            quaternion.z = rotation[2];
            quaternion.w = rotation[3];

            // mat4_cast assumes a unit quaternion; exporters commonly emit slightly off-unit values.
            const float quaternionLength = glm::length(quaternion);
            if (quaternionLength > 0.0f) {
                quaternion /= quaternionLength;
            }
            else {  // Degenerate (all-zero) rotation: treat as identity.
                quaternion.x = 0.0f;
                quaternion.y = 0.0f;
                quaternion.z = 0.0f;
                quaternion.w = 1.0f;
            }

            const glm::mat4 t =
                    glm::translate(glm::mat4(1.0f), glm::vec3(translation[0], translation[1], translation[2]));
            const glm::mat4 r = glm::mat4_cast(quaternion);
            const glm::mat4 s = glm::scale(glm::mat4(1.0f), glm::vec3(scale[0], scale[1], scale[2]));

            return t * r * s;
        }

        [[nodiscard]]
        Accessor ResolveAccessor(const std::int64_t accessorIndex) const {
            const Value& accessorValue = Lookup("accessors", accessorIndex);
            Accessor accessor;

            if (accessorValue.Find("sparse") != nullptr) {
                throw Error("Accessor ", accessorIndex, " is sparse, which is not supported.");
            }

            switch (RequireInteger(accessorValue, "componentType")) {
                case 5120: accessor.componentType = ComponentType::Byte; accessor.componentSize = 1; break;
                case 5121: accessor.componentType = ComponentType::UnsignedByte; accessor.componentSize = 1; break;
                case 5122: accessor.componentType = ComponentType::Short; accessor.componentSize = 2; break;
                case 5123: accessor.componentType = ComponentType::UnsignedShort; accessor.componentSize = 2; break;
                case 5125: accessor.componentType = ComponentType::UnsignedInt; accessor.componentSize = 4; break;
                case 5126: accessor.componentType = ComponentType::Float; accessor.componentSize = 4; break;
                default: throw Error("Accessor ", accessorIndex, " has an invalid componentType.");
            }

            const Value* const type = accessorValue.Find("type");
            if (type == nullptr || !type->IsString()) { throw Error("Accessor ", accessorIndex, " has no 'type'."); }

            const std::string& typeName = type->AsString();
            if (typeName == "SCALAR") { accessor.componentCount = 1; }
            else if (typeName == "VEC2") { accessor.componentCount = 2; }
            else if (typeName == "VEC3") { accessor.componentCount = 3; }
            else if (typeName == "VEC4") { accessor.componentCount = 4; }
            else {
                // MAT2/3/4 never carry vertex data, and their byte-padded column layout would need
                // its own decode path; anything else is invalid.
                throw Error("Accessor ", accessorIndex, " has unsupported type '", typeName, "'.");
            }

            accessor.count = static_cast<std::size_t>(RequireInteger(accessorValue, "count"));
            accessor.normalized = GetBoolean(accessorValue, "normalized", false);

            const std::size_t elementSize = accessor.componentSize * accessor.componentCount;
            accessor.stride = elementSize;

            const std::int64_t bufferViewIndex = GetInteger(accessorValue, "bufferView", -1);
            if (bufferViewIndex < 0) { return accessor; }  // No data: reads as zeros (spec-defined).

            const std::span<const std::byte> view = GetBufferViewBytes(bufferViewIndex);
            const std::size_t viewStride =
                    static_cast<std::size_t>(GetInteger(Lookup("bufferViews", bufferViewIndex), "byteStride", 0));

            if (viewStride != 0) {
                if (viewStride < elementSize) {
                    throw Error("Buffer view ", bufferViewIndex, " has a byteStride smaller than its element size.");
                }
                accessor.stride = viewStride;
            }

            const std::size_t accessorOffset = static_cast<std::size_t>(GetInteger(accessorValue, "byteOffset", 0));

            // Overflow-safe range check: the last element must end within the view, i.e.
            // offset + stride * (count - 1) + elementSize <= view.size(), tested without forming
            // the product.
            if (accessor.count != 0) {
                if (accessorOffset > view.size() || elementSize > view.size() - accessorOffset ||
                    accessor.count - 1 > (view.size() - accessorOffset - elementSize) / accessor.stride) {
                    throw Error("Accessor ", accessorIndex, " overruns buffer view ", bufferViewIndex, ".");
                }
            }

            accessor.data = view.data() + accessorOffset;

            return accessor;
        }

        // Decodes one component as float, applying the specification's normalized-integer mapping
        // when the accessor is flagged `normalized`.
        [[nodiscard]]
        static float ReadComponent(const std::byte* const source, const ComponentType type, const bool normalized) {
            switch (type) {
                case ComponentType::Float: {
                    float value = 0.0f;
                    std::memcpy(&value, source, sizeof(value));
                    return value;
                }
                case ComponentType::Byte: {
                    std::int8_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return normalized ? std::max(static_cast<float>(value) / 127.0f, -1.0f) : static_cast<float>(value);
                }
                case ComponentType::UnsignedByte: {
                    std::uint8_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return normalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
                }
                case ComponentType::Short: {
                    std::int16_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return normalized ? std::max(static_cast<float>(value) / 32767.0f, -1.0f)
                                      : static_cast<float>(value);
                }
                case ComponentType::UnsignedShort: {
                    std::uint16_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
                }
                case ComponentType::UnsignedInt: {
                    std::uint32_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return static_cast<float>(value);
                }
            }
            return 0.0f;
        }

        // Reads a whole accessor as floats: count * componentCount values, element-major.
        [[nodiscard]]
        static std::vector<float> ReadFloats(const Accessor& accessor) {
            std::vector<float> values(accessor.count * accessor.componentCount, 0.0f);
            if (accessor.data == nullptr) { return values; }

            for (std::size_t i = 0; i < accessor.count; ++i) {
                const std::byte* const element = accessor.data + i * accessor.stride;
                for (std::size_t c = 0; c < accessor.componentCount; ++c) {
                    values[i * accessor.componentCount + c] = ReadComponent(
                            element + c * accessor.componentSize, accessor.componentType, accessor.normalized);
                }
            }

            return values;
        }

        [[nodiscard]]
        std::vector<std::uint32_t> ReadIndices(const std::int64_t accessorIndex) const {
            const Accessor accessor = ResolveAccessor(accessorIndex);

            if (accessor.componentCount != 1 || accessor.normalized ||
                (accessor.componentType != ComponentType::UnsignedByte &&
                 accessor.componentType != ComponentType::UnsignedShort &&
                 accessor.componentType != ComponentType::UnsignedInt)) {
                throw Error("Index accessor ", accessorIndex,
                            " must be a non-normalized SCALAR of unsigned byte/short/int components.");
            }

            std::vector<std::uint32_t> indices(accessor.count, 0);
            if (accessor.data == nullptr) { return indices; }

            for (std::size_t i = 0; i < accessor.count; ++i) {
                const std::byte* const element = accessor.data + i * accessor.stride;

                switch (accessor.componentType) {
                    case ComponentType::UnsignedByte: {
                        std::uint8_t value = 0;
                        std::memcpy(&value, element, sizeof(value));
                        indices[i] = value;
                        break;
                    }
                    case ComponentType::UnsignedShort: {
                        std::uint16_t value = 0;
                        std::memcpy(&value, element, sizeof(value));
                        indices[i] = value;
                        break;
                    }
                    default: std::memcpy(&indices[i], element, sizeof(std::uint32_t)); break;
                }
            }

            return indices;
        }

        // Reads an optional vertex attribute accessor, checking that it has one of the accepted
        // component counts and exactly `vertexCount` elements. Returns the component count actually
        // read (0 if the attribute is absent).
        [[nodiscard]]
        std::size_t ReadAttribute(const Value& attributes, const std::string_view name,
                                  const std::initializer_list<std::size_t> acceptedComponentCounts,
                                  const std::size_t vertexCount, std::vector<float>& out) const {
            const Value* const index = attributes.Find(name);
            if (index == nullptr) { return 0; }

            const Accessor accessor = ResolveAccessor(AsNonNegativeInteger(*index, name));

            if (std::find(acceptedComponentCounts.begin(), acceptedComponentCounts.end(), accessor.componentCount) ==
                acceptedComponentCounts.end()) {
                throw Error("Attribute ", name, " has an unexpected component count (", accessor.componentCount, ").");
            }
            if (accessor.count != vertexCount) {
                throw Error("Attribute ", name, " has ", accessor.count, " elements but POSITION has ", vertexCount,
                            ".");
            }

            out = ReadFloats(accessor);
            return accessor.componentCount;
        }

        // Imports a primitive's material (once per glTF material; see m_MaterialCache) into
        // `out.materials` and returns its index there, or -1 if the primitive has no material (it
        // is then drawn with the engine's default). Imported: the metallic-roughness parameters
        // and all five texture slots, normal scale, occlusion strength, emissive factor and
        // KHR_materials_emissive_strength. Ignored: alpha mode/cutoff, double-sidedness and other
        // extensions.
        [[nodiscard]]
        std::int32_t ResolveMaterial(const Value& primitive, MeshData& out) {
            const Value* const materialIndexValue = primitive.Find("material");
            if (materialIndexValue == nullptr) { return -1; }

            const std::int64_t materialIndex = AsNonNegativeInteger(*materialIndexValue, "material");
            if (const auto cached = m_MaterialCache.find(materialIndex); cached != m_MaterialCache.end()) {
                return cached->second;
            }

            const Value& material = Lookup("materials", materialIndex);
            MaterialData data;
            data.name = GetName(material);

            MaterialParameters& parameters = data.parameters;

            if (const Value* const pbr = material.Find("pbrMetallicRoughness"); pbr != nullptr && pbr->IsObject()) {
                std::array<float, 4> baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
                if (GetNumberArray(*pbr, "baseColorFactor", baseColor)) {
                    parameters.baseColorFactor = glm::vec4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
                }
                parameters.metallicFactor = GetNumber(*pbr, "metallicFactor", parameters.metallicFactor);
                parameters.roughnessFactor = GetNumber(*pbr, "roughnessFactor", parameters.roughnessFactor);

                data.baseColorTexture = ResolveTextureSlot(pbr->Find("baseColorTexture"), true, out);
                data.metallicRoughnessTexture = ResolveTextureSlot(pbr->Find("metallicRoughnessTexture"), false, out);
            }

            if (const Value* const normal = material.Find("normalTexture"); normal != nullptr && normal->IsObject()) {
                data.normalTexture = ResolveTextureSlot(normal, false, out);
                parameters.normalScale = GetNumber(*normal, "scale", parameters.normalScale);
            }

            if (const Value* const occlusion = material.Find("occlusionTexture");
                occlusion != nullptr && occlusion->IsObject()) {
                data.occlusionTexture = ResolveTextureSlot(occlusion, false, out);
                parameters.occlusionStrength = GetNumber(*occlusion, "strength", parameters.occlusionStrength);
            }

            data.emissiveTexture = ResolveTextureSlot(material.Find("emissiveTexture"), true, out);

            std::array<float, 3> emissive = {0.0f, 0.0f, 0.0f};
            if (GetNumberArray(material, "emissiveFactor", emissive)) {
                parameters.emissiveFactor = glm::vec3(emissive[0], emissive[1], emissive[2]);
            }
            if (const Value* const extensions = material.Find("extensions")) {
                if (const Value* const strength = extensions->Find("KHR_materials_emissive_strength")) {
                    parameters.emissiveFactor *= GetNumber(*strength, "emissiveStrength", 1.0f);
                }
            }

            if (out.materials.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                throw Error("Model has too many materials.");
            }

            const auto result = static_cast<std::int32_t>(out.materials.size());
            out.materials.push_back(std::move(data));
            m_MaterialCache.emplace(materialIndex, result);

            return result;
        }

        // Resolves a textureInfo object ({index, texCoord, ...}) to an index into out.textures, or
        // -1 if absent. `srgb` says whether the image holds color (base color, emissive) or data.
        [[nodiscard]]
        std::int32_t ResolveTextureSlot(const Value* const textureInfo, const bool srgb, MeshData& out) {
            if (textureInfo == nullptr || !textureInfo->IsObject()) { return -1; }

            // Only the first UV set is imported (Vertex has one), so a texture mapped through
            // another set cannot be displayed correctly and is left off rather than misapplied.
            if (GetInteger(*textureInfo, "texCoord", 0) != 0) { return -1; }

            return ResolveTexture(RequireInteger(*textureInfo, "index"), srgb, out);
        }

        // Decodes the image behind glTF texture `textureIndex` (once per color space; see
        // m_TextureCache) and appends it to `out.textures`. Returns its index there, or -1 if the
        // texture has no usable image source (e.g. only a compressed-format extension source).
        [[nodiscard]]
        std::int32_t ResolveTexture(const std::int64_t textureIndex, const bool srgb, MeshData& out) {
            // The same image may in principle be used both as color and as data; the two need
            // different GPU formats, hence the color space is part of the key.
            const std::int64_t cacheKey = textureIndex * 2 + (srgb ? 1 : 0);
            if (const auto cached = m_TextureCache.find(cacheKey); cached != m_TextureCache.end()) {
                return cached->second;
            }

            const Value& texture = Lookup("textures", textureIndex);
            std::int32_t result = -1;

            if (const Value* const source = texture.Find("source")) {
                const std::int64_t imageIndex = AsNonNegativeInteger(*source, "source");

                if (out.textures.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                    throw Error("Model has too many textures.");
                }

                TextureData data;
                data.image = LoadImage(imageIndex);
                data.settings = ReadSampler(texture);
                data.settings.srgb = srgb;
                data.name = GetName(texture);
                if (data.name.empty()) { data.name = GetName(Lookup("images", imageIndex)); }

                result = static_cast<std::int32_t>(out.textures.size());
                out.textures.push_back(std::move(data));
            }

            m_TextureCache.emplace(cacheKey, result);
            return result;
        }

        // Fetches and decodes glTF image `imageIndex`, whichever way it is stored.
        [[nodiscard]]
        ImageData LoadImage(const std::int64_t imageIndex) const {
            const Value& image = Lookup("images", imageIndex);
            const std::string debugName = "image " + std::to_string(imageIndex);

            if (const Value* const bufferViewIndex = image.Find("bufferView")) {
                const std::span<const std::byte> bytes =
                        GetBufferViewBytes(AsNonNegativeInteger(*bufferViewIndex, "bufferView"));
                return ImageImporter::Decode(bytes, debugName);
            }

            const Value* const uri = image.Find("uri");
            if (uri == nullptr || !uri->IsString()) {
                throw Error("Image ", imageIndex, " has neither a 'bufferView' nor a 'uri'.");
            }

            const std::vector<std::byte> bytes = ResolveUri(uri->AsString(), m_BaseDirectory);
            return ImageImporter::Decode(bytes, debugName + " (" + uri->AsString() + ")");
        }

        // The bytes of buffer `bufferIndex`, exactly its declared byteLength: the GLB BIN chunk
        // for a buffer without a uri, otherwise the data: URI or external file the uri names,
        // fetched on first use and kept for the rest of the import.
        [[nodiscard]]
        std::span<const std::byte> GetBufferBytes(const std::int64_t bufferIndex) const {
            const Value& buffer = Lookup("buffers", bufferIndex);
            const std::size_t declaredLength = static_cast<std::size_t>(RequireInteger(buffer, "byteLength"));

            std::span<const std::byte> bytes = m_Bin;
            std::string source = "the GLB BIN chunk";

            if (const Value* const uri = buffer.Find("uri")) {
                if (!uri->IsString()) { throw Error("Buffer ", bufferIndex, " has a non-string 'uri'."); }

                auto loaded = m_ExternalBuffers.find(bufferIndex);
                if (loaded == m_ExternalBuffers.end()) {
                    loaded = m_ExternalBuffers.emplace(bufferIndex, ResolveUri(uri->AsString(), m_BaseDirectory)).first;
                }

                bytes = loaded->second;
                source = uri->AsString().starts_with("data:") ? "its data: URI" : "'" + uri->AsString() + "'";
            }
            else if (m_Bin.empty()) {
                throw Error("Buffer ", bufferIndex, " has no 'uri' and the file has no GLB BIN chunk to hold it.");
            }

            if (bytes.size() < declaredLength) {
                throw Error("Buffer ", bufferIndex, " declares ", declaredLength, " bytes but ", source, " holds only ",
                            bytes.size(), " (missing or corrupt data?).");
            }

            return bytes.first(declaredLength);
        }

        // The bytes of a whole buffer view, bounds-checked against its buffer.
        [[nodiscard]]
        std::span<const std::byte> GetBufferViewBytes(const std::int64_t bufferViewIndex) const {
            const Value& bufferView = Lookup("bufferViews", bufferViewIndex);
            const std::int64_t bufferIndex = RequireInteger(bufferView, "buffer");
            const std::span<const std::byte> buffer = GetBufferBytes(bufferIndex);

            const std::size_t viewOffset = static_cast<std::size_t>(GetInteger(bufferView, "byteOffset", 0));
            const std::size_t viewLength = static_cast<std::size_t>(RequireInteger(bufferView, "byteLength"));

            if (viewOffset > buffer.size() || viewLength > buffer.size() - viewOffset) {
                throw Error("Buffer view ", bufferViewIndex, " lies outside buffer ", bufferIndex, ".");
            }

            return buffer.subspan(viewOffset, viewLength);
        }

        // Converts a texture's sampler (or glTF's defaults if it has none) to TextureSettings.
        [[nodiscard]]
        TextureSettings ReadSampler(const Value& texture) const {
            TextureSettings settings;  // Defaults already match glTF's (linear, repeat).

            const Value* const samplerIndex = texture.Find("sampler");
            if (samplerIndex == nullptr) { return settings; }

            const Value& sampler = Lookup("samplers", AsNonNegativeInteger(*samplerIndex, "sampler"));

            // OpenGL enumerants, as used by glTF.
            constexpr std::int64_t kNearest = 9728;
            constexpr std::int64_t kLinear = 9729;
            constexpr std::int64_t kNearestMipmapNearest = 9984;
            constexpr std::int64_t kLinearMipmapNearest = 9985;
            constexpr std::int64_t kNearestMipmapLinear = 9986;
            constexpr std::int64_t kLinearMipmapLinear = 9987;
            constexpr std::int64_t kClampToEdge = 33071;
            constexpr std::int64_t kMirroredRepeat = 33648;
            constexpr std::int64_t kRepeat = 10497;

            if (GetInteger(sampler, "magFilter", kLinear) == kNearest) {
                settings.magFilter = TextureFilter::Nearest;
            }

            switch (GetInteger(sampler, "minFilter", kLinearMipmapLinear)) {
                case kNearest:
                    settings.minFilter = TextureFilter::Nearest;
                    settings.mipmapMode = TextureMipmapMode::None;
                    break;
                case kLinear:
                    settings.minFilter = TextureFilter::Linear;
                    settings.mipmapMode = TextureMipmapMode::None;
                    break;
                case kNearestMipmapNearest:
                    settings.minFilter = TextureFilter::Nearest;
                    settings.mipmapMode = TextureMipmapMode::Nearest;
                    break;
                case kLinearMipmapNearest:
                    settings.minFilter = TextureFilter::Linear;
                    settings.mipmapMode = TextureMipmapMode::Nearest;
                    break;
                case kNearestMipmapLinear:
                    settings.minFilter = TextureFilter::Nearest;
                    settings.mipmapMode = TextureMipmapMode::Linear;
                    break;
                default:  // kLinearMipmapLinear, and anything invalid.
                    settings.minFilter = TextureFilter::Linear;
                    settings.mipmapMode = TextureMipmapMode::Linear;
                    break;
            }

            auto toWrap = [](const std::int64_t value) {
                switch (value) {
                    case kClampToEdge: return TextureWrap::ClampToEdge;
                    case kMirroredRepeat: return TextureWrap::MirroredRepeat;
                    default: return TextureWrap::Repeat;  // kRepeat, and anything invalid.
                }
            };
            settings.wrapU = toWrap(GetInteger(sampler, "wrapS", kRepeat));
            settings.wrapV = toWrap(GetInteger(sampler, "wrapT", kRepeat));

            return settings;
        }

        [[nodiscard]]
        static std::string GetName(const Value& object) {
            const Value* const name = object.Find("name");
            return name != nullptr && name->IsString() ? name->AsString() : std::string();
        }

        void AppendMesh(const std::int64_t meshIndex, const std::string& nodeName, const glm::mat4& world,
                        MeshData& out) {
            const Value& mesh = Lookup("meshes", meshIndex);
            const std::string name = !nodeName.empty() ? nodeName : GetName(mesh);

            const Value* const primitives = mesh.Find("primitives");
            if (primitives == nullptr || !primitives->IsArray()) {
                throw Error("Mesh ", meshIndex, " has no 'primitives' array.");
            }

            for (std::size_t i = 0; i < primitives->AsArray().size(); ++i) {
                const Value& primitive = primitives->AsArray()[i];
                if (!primitive.IsObject()) { throw Error("Mesh ", meshIndex, " primitive ", i, " is not an object."); }

                AppendPrimitive(primitive, name, world, out);
            }
        }

        void AppendPrimitive(const Value& primitive, const std::string& name, const glm::mat4& world, MeshData& out) {
            const auto mode = static_cast<PrimitiveMode>(GetInteger(primitive, "mode", 4));
            if (mode != PrimitiveMode::Triangles && mode != PrimitiveMode::TriangleStrip &&
                mode != PrimitiveMode::TriangleFan) {
                return;  // Points and lines are not triangle geometry; see the header comment.
            }

            const Value* const attributes = primitive.Find("attributes");
            if (attributes == nullptr || !attributes->IsObject()) {
                throw Error("Primitive has no 'attributes' object.");
            }

            // Without positions there is nothing to draw (the specification says to skip the
            // primitive).
            const Value* const positionIndex = attributes->Find("POSITION");
            if (positionIndex == nullptr) { return; }

            const Accessor positionAccessor = ResolveAccessor(AsNonNegativeInteger(*positionIndex, "POSITION"));
            if (positionAccessor.componentCount != 3) { throw Error("POSITION attribute is not a VEC3."); }

            const std::size_t vertexCount = positionAccessor.count;
            if (vertexCount == 0) { return; }

            const std::vector<float> positions = ReadFloats(positionAccessor);

            std::vector<float> normals;
            std::vector<float> tangents;
            std::vector<float> uvs;
            std::vector<float> colors;
            const bool hasNormals = ReadAttribute(*attributes, "NORMAL", {3}, vertexCount, normals) != 0;
            // Tangents without normals would be meaningless (spec: TANGENT requires NORMAL); the
            // flat-normal path below generates both.
            const bool hasTangents =
                    hasNormals && ReadAttribute(*attributes, "TANGENT", {4}, vertexCount, tangents) != 0;
            const bool hasUvs = ReadAttribute(*attributes, "TEXCOORD_0", {2}, vertexCount, uvs) != 0;
            const std::size_t colorComponents = ReadAttribute(*attributes, "COLOR_0", {3, 4}, vertexCount, colors);

            // Indices (or the implicit 0..n-1 sequence for non-indexed geometry), expanded to a
            // triangle list and validated against the vertex count before any of them is used.
            std::vector<std::uint32_t> indices;
            if (const Value* const indicesIndex = primitive.Find("indices")) {
                indices = ReadIndices(AsNonNegativeInteger(*indicesIndex, "indices"));
            }
            else {
                if (vertexCount > std::numeric_limits<std::uint32_t>::max()) {
                    throw Error("Primitive has too many vertices (", vertexCount, ").");
                }
                indices.resize(vertexCount);
                std::iota(indices.begin(), indices.end(), 0u);
            }

            for (const std::uint32_t index : indices) {
                if (index >= vertexCount) {
                    throw Error("Primitive index ", index, " is out of range (", vertexCount, " vertices).");
                }
            }

            indices = ToTriangleList(indices, mode);
            if (indices.empty()) { return; }

            const std::int32_t materialIndex = ResolveMaterial(primitive, out);

            // Assemble local-space vertices in the engine's layout.
            std::vector<Vertex> vertices(vertexCount);
            for (std::size_t i = 0; i < vertexCount; ++i) {
                Vertex& vertex = vertices[i];
                vertex.position = glm::vec3(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);

                if (hasNormals) {
                    vertex.normal = glm::vec3(normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2]);
                }

                if (!uvs.empty()) { vertex.uv = glm::vec2(uvs[i * 2 + 0], uvs[i * 2 + 1]); }

                if (colorComponents == 3) {
                    vertex.color = glm::vec4(colors[i * 3 + 0], colors[i * 3 + 1], colors[i * 3 + 2], 1.0f);
                }
                else if (colorComponents == 4) {
                    vertex.color =
                            glm::vec4(colors[i * 4 + 0], colors[i * 4 + 1], colors[i * 4 + 2], colors[i * 4 + 3]);
                }

                if (hasTangents) {
                    vertex.tangent = glm::vec4(tangents[i * 4 + 0], tangents[i * 4 + 1], tangents[i * 4 + 2],
                                               tangents[i * 4 + 3]);
                }
            }

            // No normals: the specification calls for flat shading, which needs one vertex per
            // triangle corner. De-index the primitive and assign each face's geometric normal.
            if (!hasNormals) {
                std::vector<Vertex> flatVertices;
                flatVertices.reserve(indices.size());

                for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                    Vertex a = vertices[indices[i + 0]];
                    Vertex b = vertices[indices[i + 1]];
                    Vertex c = vertices[indices[i + 2]];

                    const glm::vec3 faceNormal =
                            SafeNormalize(glm::cross(b.position - a.position, c.position - a.position));
                    a.normal = faceNormal;
                    b.normal = faceNormal;
                    c.normal = faceNormal;

                    flatVertices.push_back(a);
                    flatVertices.push_back(b);
                    flatVertices.push_back(c);
                }

                vertices = std::move(flatVertices);
                std::iota(indices.begin(), indices.end(), 0u);
            }

            // Normal mapping needs a tangent frame. Files rarely carry TANGENT; derive it from the
            // UV layout when they do not (and there is a UV layout to derive it from).
            if (!hasTangents && hasUvs) { GenerateTangents(vertices, indices); }

            // Bake the node's world transform. Positions take the full matrix; normals take the
            // inverse-transpose of its linear part so non-uniform scale keeps them perpendicular
            // (falling back to the linear part itself if the matrix is singular, where no correct
            // normal exists anyway); tangents are surface directions and take the linear part.
            const glm::mat3 linear(world);
            const float determinant = glm::determinant(linear);
            const glm::mat3 normalMatrix = determinant != 0.0f ? glm::inverseTranspose(linear) : linear;

            for (Vertex& vertex : vertices) {
                vertex.position = glm::vec3(world * glm::vec4(vertex.position, 1.0f));
                vertex.normal = SafeNormalize(normalMatrix * vertex.normal);
                vertex.tangent = glm::vec4(SafeNormalize(linear * glm::vec3(vertex.tangent)), vertex.tangent.w);
            }

            // A mirroring transform turns counter-clockwise triangles clockwise; swap two corners
            // of every triangle so front faces stay front faces under back-face culling. It also
            // flips the handedness of every tangent frame.
            if (determinant < 0.0f) {
                for (std::size_t i = 0; i + 2 < indices.size(); i += 3) { std::swap(indices[i + 1], indices[i + 2]); }
                for (Vertex& vertex : vertices) { vertex.tangent.w = -vertex.tangent.w; }
            }

            // Append as one sub-mesh. Its indices stay relative to its own first vertex; the
            // sub-mesh's vertexOffset rebases them at draw time.
            if (out.vertices.size() + vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                throw Error("Model has too many vertices for a single mesh.");
            }
            if (out.indices.size() + indices.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw Error("Model has too many indices for a single mesh.");
            }

            out.subMeshes.push_back(SubMesh{.firstIndex = static_cast<std::uint32_t>(out.indices.size()),
                                            .indexCount = static_cast<std::uint32_t>(indices.size()),
                                            .vertexOffset = static_cast<std::int32_t>(out.vertices.size()),
                                            .name = name,
                                            .materialIndex = materialIndex});

            out.vertices.insert(out.vertices.end(), vertices.begin(), vertices.end());
            out.indices.insert(out.indices.end(), indices.begin(), indices.end());
        }

        const Value& m_Root;
        std::span<const std::byte> m_Bin;
        std::filesystem::path m_BaseDirectory;
        // Buffers fetched by uri, by buffer index. A cache (hence mutable): accessor decoding is
        // otherwise read-only, and every accessor of a .gltf would reload its .bin without it.
        mutable std::unordered_map<std::int64_t, std::vector<std::byte>> m_ExternalBuffers;
        // glTF texture index (and color space; see ResolveTexture) -> MeshData::textures index, or
        // -1 if it resolved to nothing usable, so a texture shared by several materials is decoded
        // and uploaded once. Likewise glTF material index -> MeshData::materials index.
        std::unordered_map<std::int64_t, std::int32_t> m_TextureCache;
        std::unordered_map<std::int64_t, std::int32_t> m_MaterialCache;
    };

}  // namespace

namespace Frost {

    MeshData GltfImporter::Import(const std::filesystem::path& path) {
        try {
            const std::vector<std::byte> bytes = ReadFile(path);
            const GltfContents contents = ParseContainer(bytes);
            const Value root = Json::Parse(contents.json);

            if (!root.IsObject()) { throw Error("glTF JSON root is not an object."); }

            Document document(root, contents.bin, path.parent_path());
            document.CheckCompatibility();

            MeshData data;
            document.ImportGeometry(data);

            if (data.subMeshes.empty()) { throw Error("File contains no triangle geometry."); }

            return data;
        }
        catch (const std::runtime_error& e) {  // ImportError, or a JSON parse error.
            throw ImportError("Failed to import glTF file '" + path.string() + "': " + e.what());
        }
    }

}  // namespace Frost
