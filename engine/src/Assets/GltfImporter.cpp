#include <Frost/Assets/GltfImporter.hpp>

#include <Frost/Animation/AnimationClip.hpp>
#include <Frost/Animation/Skeleton.hpp>
#include <Frost/Animation/SkinnedMesh.hpp>
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
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Byte-order note: glTF binary data is little-endian and is read here with memcpy into host
// integers/floats, i.e. the importer assumes a little-endian host (as does the rest of the engine,
// e.g. SPIR-V loading).

namespace {

    using Frost::GltfModelInfo;
    using Frost::ImageData;
    using Frost::ImageImporter;
    using Frost::ImportError;
    using Frost::Json::Value;
    using Frost::MaterialData;
    using Frost::MaterialParameters;
    using Frost::MeshData;
    using Frost::SkeletalMeshData;
    using Frost::SkinWeights;
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

    // A glTF rotation (x, y, z, w) as a unit quaternion. Assigned member-wise: glm::quat's
    // constructor argument order depends on a configuration macro. mat4_cast and the animation
    // system assume unit quaternions, and exporters commonly emit slightly off-unit values; a
    // degenerate (all-zero) rotation is treated as the identity.
    [[nodiscard]]
    glm::quat NormalizedRotation(const float x, const float y, const float z, const float w) noexcept {
        glm::quat quaternion = Frost::MakeQuaternion(x, y, z, w);
        const float length = glm::length(quaternion);
        if (length > 0.0f && std::isfinite(length)) { return quaternion / length; }
        return Frost::IdentityQuaternion();
    }

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

    // The words of a name, lowercased, for matching parts to bones: split at non-alphanumeric
    // characters, between a lowercase and an uppercase letter, before the last capital of a run of
    // capitals followed by a lowercase letter, and between letters and digits ("AK12Mag" -> "ak",
    // "12", "mag"; "MagEquipped" -> "mag", "equipped"; "HTTPServer" -> "http", "server"). Numbers and
    // single letters are dropped, and so are words that describe what a thing is rather than
    // which thing it is ("mesh", "bone", ...).
    [[nodiscard]]
    std::vector<std::string> NameWords(const std::string& name) {
        static constexpr std::array<std::string_view, 16> kGeneric = {
                "mesh", "geo", "geometry", "model", "node", "bone", "joint", "jnt",
                "obj", "object", "part", "def", "mch", "ctrl", "lod", "skin"};

        const auto kind = [](const char c) {
            const auto u = static_cast<unsigned char>(c);
            if (std::islower(u) != 0) { return 1; }
            if (std::isupper(u) != 0) { return 2; }
            if (std::isdigit(u) != 0) { return 3; }
            return 0;
        };

        std::vector<std::string> words;
        std::string word;
        const auto finish = [&] {
            const bool number = !word.empty() && std::all_of(word.begin(), word.end(), [](const char c) {
                return std::isdigit(static_cast<unsigned char>(c)) != 0;
            });
            std::string lower;
            for (const char c : word) { lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); }
            if (lower.size() >= 2 && !number && std::find(kGeneric.begin(), kGeneric.end(), lower) == kGeneric.end()) {
                words.push_back(lower);
            }
            word.clear();
        };

        for (std::size_t i = 0; i < name.size(); ++i) {
            const int current = kind(name[i]);
            if (current == 0) {
                finish();
                continue;
            }
            if (!word.empty()) {
                const int previous = kind(word.back());
                const bool lowerToUpper = previous == 1 && current == 2;
                const bool capitalsToWord = previous == 2 && current == 2 && i + 1 < name.size() && kind(name[i + 1]) == 1;
                const bool letterDigit = (previous == 3) != (current == 3);
                if (lowerToUpper || capitalsToWord || letterDigit) { finish(); }
            }
            word.push_back(name[i]);
        }
        finish();
        return words;
    }

    class Document final {
    private:
        // --- Types used throughout (declared first: member signatures refer to them) ---------------

        struct NodeInfo {
            std::int64_t parent = -1;
            std::vector<std::int64_t> children;
            glm::mat4 local{1.0f};      // In the file's units (meters).
            Frost::BoneTransform pose;  // The same, as translation/rotation/scale in centimeters.
            // The node's name, made unique among the file's nodes (unnamed nodes are "Node<index>";
            // a repeated name gets "_<index>" appended). Bones are named after their nodes.
            std::string name;
        };

        struct SkinInfo {
            std::vector<std::int64_t> joints;     // Node indices, unique.
            std::vector<glm::mat4> inverseBind;  // Per joint, in meters (identity if the file has none).
        };

        struct SkeletonBuild {
            Frost::SkeletonData skeleton;
            std::vector<std::int32_t> nodeToBone;  // Per node: its bone, or -1.
            std::vector<std::int64_t> boneToNode;
            bool fromSkins = false;
        };

        enum class ChannelPath { Translation, Rotation, Scale, Weights };

        struct AnimationChannelInfo {
            std::int64_t node = 0;
            std::int64_t sampler = 0;
            ChannelPath path = ChannelPath::Translation;
        };

        // A sampler's keyframes, decoded: times, and per key `componentCount` floats (three times
        // that for cubic splines: in-tangent, value, out-tangent).
        struct Sampler {
            Frost::AnimationInterpolation interpolation = Frost::AnimationInterpolation::Linear;
            std::vector<float> times;
            std::vector<float> values;
            std::size_t componentCount = 3;
        };

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

        // --- Static mesh ------------------------------------------------------------------------

        // Walks the scene graph and appends every triangle primitive reached to `out`, with each
        // node's world transform baked into the vertices: the model as posed in the file. A skinned
        // mesh is posed by its skin instead (its joints at their place in the file), since the
        // specification says a skinned mesh's own node transform does not apply to it.
        void ImportStaticMesh(MeshData& out) {
            ResetCaches();

            VisitMeshNodes([&](const std::int64_t nodeIndex, const Value& node, const std::int64_t meshIndex,
                               const glm::mat4& world) {
                const std::string name = PartName(node, meshIndex);

                // A skin that cannot be used (malformed, or not there) leaves the mesh placed by its
                // node, as skins were ignored before they were supported.
                const SkinInfo* skin = nullptr;
                try {
                    if (const std::optional<std::size_t> skinIndex = GetNodeSkin(node, nodeIndex)) {
                        skin = &GetSkins().at(*skinIndex);
                    }
                }
                catch (const ImportError&) {
                    skin = nullptr;
                }

                if (skin != nullptr) {
                    AppendPosedSkinnedMesh(meshIndex, *skin, name, world, out);
                    return;
                }

                for (PrimitiveGeometry& geometry : ReadMesh(meshIndex, out, std::nullopt)) {
                    BakeTransform(geometry, world);
                    AppendGeometry(geometry, name, out);
                }
            });
        }

        // --- Skeleton ---------------------------------------------------------------------------

        // The skeleton the file's skins (or, without skins, its node hierarchy) make; see
        // GltfModel::ImportSkeleton.
        [[nodiscard]]
        Frost::SkeletonData ImportSkeleton() {
            return GetSkeleton().skeleton;
        }

        // --- Skeletal mesh -----------------------------------------------------------------------

        // Every mesh in the scene, bound to `target` (by bone name): skinned meshes by their skins,
        // rigid ones attached to bones as `options` says. See GltfModel::ImportSkeletalMesh.
        [[nodiscard]]
        SkeletalMeshData ImportSkeletalMesh(const Frost::SkeletonData& target, std::vector<std::string>& warnings,
                                            const Frost::GltfAttachmentOptions& options,
                                            std::vector<Frost::GltfSeparatePart>* const separateParts) {
            ResetCaches();

            const SkeletonBuild& build = GetSkeleton();
            const glm::mat4 metersToWorld = glm::scale(glm::mat4(1.0f), glm::vec3(Frost::kMeter));

            // The target's rest pose, for attachments to bones the file does not have.
            const std::vector<glm::mat4> targetRest = target.ComputeModelTransforms(target.GetRestPose());

            SkeletalMeshData result;
            std::unordered_map<std::string, std::uint16_t> bindIndices;  // Bone name -> bind table entry.
            std::vector<glm::mat4> restSkinMatrices;  // Per entry: its skinning matrix in the file's pose.
            std::vector<std::string> conflicts;
            std::vector<std::string> missingAttachmentBones;

            // The bind table entry of a bone (by name), added on first use; nullopt if `target` has
            // no such bone. A bone of the file's skeleton takes its inverse bind matrix from the file
            // (see BoneInverseBind); one only the target has is bound in the target's rest pose.
            const auto bindEntry = [&](const std::string& name) -> std::optional<std::uint16_t> {
                if (const auto found = bindIndices.find(name); found != bindIndices.end()) { return found->second; }

                const std::optional<std::size_t> targetBone = target.FindBone(name);
                if (!targetBone.has_value()) { return std::nullopt; }
                if (result.bindBones.size() >= std::size_t{65536}) { throw Error("Model is bound to too many bones."); }

                glm::mat4 inverseBind(1.0f);
                glm::mat4 restSkin(1.0f);
                if (const std::optional<std::size_t> fileBone = build.skeleton.FindBone(name)) {
                    const auto bone = static_cast<std::int32_t>(*fileBone);
                    inverseBind = BoneInverseBind(bone);
                    restSkin = WorldCentimeters(build.boneToNode[*fileBone]) * inverseBind;
                }
                else {
                    inverseBind = glm::inverse(targetRest[*targetBone]);
                }

                const auto index = static_cast<std::uint16_t>(result.bindBones.size());
                result.bindBones.push_back(name);
                result.inverseBindMatrices.push_back(inverseBind);
                restSkinMatrices.push_back(restSkin);
                bindIndices.emplace(name, index);
                return index;
            };

            // A rigid primitive of node `nodeIndex`, part `partName`: attached as a whole to the
            // bone the options choose (its vertices stored so that bone's skinning matrix puts them
            // where the file has them), or left where the file has it, moving with nothing.
            // The bone a rigid part goes with: the one chosen for it, else the one above it in the
            // file, else (for a loose part) a suggested one; empty for none.
            const auto chooseBone = [&](const std::int64_t nodeIndex, const std::string& partName) -> std::string {
                if (const auto chosen = options.partBones.find(partName); chosen != options.partBones.end()) {
                    return chosen->second;
                }
                if (const std::optional<std::int32_t> parent = BoneAtOrAbove(nodeIndex)) {
                    return build.skeleton.bones[static_cast<std::size_t>(*parent)].name;
                }
                return options.attachLooseParts ? SuggestBone(partName) : std::string();
            };

            const auto attachRigidly = [&](PrimitiveGeometry& geometry, const std::int64_t nodeIndex,
                                           const std::string& partName) -> std::int32_t {
                const glm::mat4 nodeWorld = WorldCentimeters(nodeIndex) * metersToWorld;
                geometry.skin.assign(geometry.vertices.size(), SkinWeights{});

                const std::string bone = chooseBone(nodeIndex, partName);

                std::optional<std::uint16_t> entry;
                if (!bone.empty()) {
                    entry = bindEntry(bone);
                    if (!entry.has_value()) { missingAttachmentBones.push_back("'" + partName + "' to '" + bone + "'"); }
                }
                if (!entry.has_value()) {
                    BakeTransform(geometry, nodeWorld);
                    return -1;
                }

                BakeTransform(geometry, glm::inverse(restSkinMatrices[*entry]) * nodeWorld);
                return *entry;
            };

            VisitMeshNodes([&](const std::int64_t nodeIndex, const Value& node, const std::int64_t meshIndex,
                               const glm::mat4& /*world*/) {
                const std::string name = PartName(node, meshIndex);
                const std::optional<std::size_t> skinIndex = GetNodeSkin(node, nodeIndex);
                const SkinInfo* const skin = skinIndex.has_value() ? &GetSkins().at(*skinIndex) : nullptr;

                // A part imported on its own: where the file has it, in the skeletal mesh's space.
                if (skin == nullptr && separateParts != nullptr && options.separateParts.contains(name)) {
                    auto part = std::find_if(separateParts->begin(), separateParts->end(),
                                             [&](const Frost::GltfSeparatePart& existing) { return existing.name == name; });
                    if (part == separateParts->end()) {
                        std::string bone = chooseBone(nodeIndex, name);
                        if (!bone.empty() && !target.FindBone(bone).has_value()) {
                            missingAttachmentBones.push_back("'" + name + "' to '" + bone + "'");
                            bone.clear();
                        }
                        separateParts->push_back(Frost::GltfSeparatePart{.name = name, .bone = std::move(bone), .mesh = {}});
                        part = std::prev(separateParts->end());
                    }

                    const glm::mat4 nodeWorld = WorldCentimeters(nodeIndex) * metersToWorld;
                    for (PrimitiveGeometry& geometry : ReadMesh(meshIndex, result.mesh, std::nullopt)) {
                        BakeTransform(geometry, nodeWorld);
                        AppendGeometry(geometry, name, part->mesh);
                    }
                    return;
                }

                // Per joint of the skin, its bind table entry (-1: not needed yet).
                std::vector<std::int32_t> jointEntries(skin != nullptr ? skin->joints.size() : 0, -1);

                const std::optional<std::size_t> jointCount =
                        skin != nullptr ? std::optional<std::size_t>(skin->joints.size()) : std::nullopt;
                for (PrimitiveGeometry& geometry : ReadMesh(meshIndex, result.mesh, jointCount)) {
                    std::int32_t attachment = -1;
                    if (skin == nullptr || geometry.skin.empty()) {
                        attachment = attachRigidly(geometry, nodeIndex, name);
                    }
                    else {
                        // Skinned: the vertices are already in the skin's bind space (only the unit
                        // changes); the influences' joints become bind table entries.
                        BakeTransform(geometry, metersToWorld);
                        for (SkinWeights& weights : geometry.skin) {
                            for (std::size_t i = 0; i < SkinWeights::kMaxInfluences; ++i) {
                                if (weights.weights[i] == 0.0f) { continue; }

                                const std::size_t joint = weights.bones[i];
                                if (jointEntries[joint] < 0) {
                                    const std::int64_t jointNode = skin->joints[joint];
                                    const std::int32_t bone = build.nodeToBone[static_cast<std::size_t>(jointNode)];
                                    const std::string& boneName = build.skeleton.bones[static_cast<std::size_t>(bone)].name;
                                    const std::optional<std::uint16_t> entry = bindEntry(boneName);
                                    if (!entry.has_value()) {
                                        throw Error("The skeleton has no bone '", boneName, "', which the mesh is skinned to.");
                                    }
                                    jointEntries[joint] = *entry;

                                    // Another skin may already have bound this joint in a different
                                    // pose; the table holds one inverse bind matrix per bone.
                                    if (!NearlyEqual(BoneInverseBind(bone), ToCentimeters(skin->inverseBind[joint]))) {
                                        conflicts.push_back(GetNodes()[static_cast<std::size_t>(jointNode)].name);
                                    }
                                }
                                weights.bones[i] = static_cast<std::uint16_t>(jointEntries[joint]);
                            }
                        }
                    }

                    AppendGeometry(geometry, name, result.mesh);
                    result.skin.insert(result.skin.end(), geometry.skin.begin(), geometry.skin.end());
                    result.attachments.push_back(attachment);
                }
            });

            if (result.mesh.subMeshes.empty()) {
                if (separateParts != nullptr && !separateParts->empty()) {
                    throw Error("Every part of the model is to be imported on its own: the skeletal mesh would have "
                                "nothing left to draw.");
                }
                throw Error("File contains no triangle geometry.");
            }

            if (!conflicts.empty()) {
                std::sort(conflicts.begin(), conflicts.end());
                conflicts.erase(std::unique(conflicts.begin(), conflicts.end()), conflicts.end());
                warnings.push_back("Several skins bind " + JoinNames(conflicts) +
                                   " in different poses; the first skin's bind pose is used, so parts bound by the "
                                   "others may be posed incorrectly.");
            }
            if (!missingAttachmentBones.empty()) {
                std::sort(missingAttachmentBones.begin(), missingAttachmentBones.end());
                missingAttachmentBones.erase(std::unique(missingAttachmentBones.begin(), missingAttachmentBones.end()),
                                             missingAttachmentBones.end());
                std::string list;
                for (const std::string& entry : missingAttachmentBones) { list += (list.empty() ? "" : ", ") + entry; }
                warnings.push_back("Could not attach " + list + ": the skeleton has no such bone, so those parts do not "
                                   "move with it.");
            }
            return result;
        }

        // --- Animations --------------------------------------------------------------------------

        // glTF animation `index` as a clip for `target`: each channel that animates a node whose
        // (file) name is a bone of the skeleton becomes part of that bone's track. See
        // GltfModel::ImportAnimation.
        [[nodiscard]]
        Frost::AnimationClipData ImportAnimation(const std::size_t index, const Frost::SkeletonData& target,
                                                 std::vector<std::string>& warnings) {
            const Value& animation = Lookup("animations", static_cast<std::int64_t>(index));
            const std::string animationName = AnimationDisplayName(index);

            Frost::AnimationClipData clip;
            std::map<std::string, std::size_t> trackIndices;  // Bone name -> index in clip.tracks.
            std::vector<std::string> unknownNodes;
            std::size_t morphChannels = 0;

            for (const AnimationChannelInfo& channel : ReadAnimationChannels(animation, index)) {
                if (channel.path == ChannelPath::Weights) {
                    ++morphChannels;
                    continue;
                }

                const std::string& boneName = GetNodes()[static_cast<std::size_t>(channel.node)].name;
                if (!target.FindBone(boneName).has_value()) {
                    unknownNodes.push_back(boneName);
                    continue;
                }

                auto [found, inserted] = trackIndices.try_emplace(boneName, clip.tracks.size());
                if (inserted) { clip.tracks.push_back(Frost::BoneTrack{.bone = boneName, .translation = {}, .rotation = {}, .scale = {}}); }
                Frost::BoneTrack& track = clip.tracks[found->second];

                const Sampler sampler = ReadSampler(animation, channel.sampler, channel.path, index);
                if (!sampler.times.empty()) { clip.duration = std::max(clip.duration, sampler.times.back()); }

                bool replaced = false;
                switch (channel.path) {
                    case ChannelPath::Translation:
                        replaced = !track.translation.IsEmpty();
                        track.translation = ToVectorChannel(sampler, Frost::kMeter);
                        break;
                    case ChannelPath::Rotation:
                        replaced = !track.rotation.IsEmpty();
                        track.rotation = ToRotationChannel(sampler);
                        break;
                    case ChannelPath::Scale:
                        replaced = !track.scale.IsEmpty();
                        track.scale = ToVectorChannel(sampler, 1.0f);
                        break;
                    case ChannelPath::Weights: break;
                }
                if (replaced) {
                    warnings.push_back("Animation '" + animationName + "' animates the same property of '" + boneName +
                                       "' twice; the later channel is used.");
                }
            }

            if (clip.tracks.empty()) {
                throw Error("Animation '", animationName, "' does not move any bone of the skeleton",
                            unknownNodes.empty() ? "." : " (it animates " + JoinNames(unknownNodes) + ").");
            }
            if (!unknownNodes.empty()) {
                std::sort(unknownNodes.begin(), unknownNodes.end());
                unknownNodes.erase(std::unique(unknownNodes.begin(), unknownNodes.end()), unknownNodes.end());
                warnings.push_back("Animation '" + animationName + "' also animates " + JoinNames(unknownNodes) +
                                   ", which the skeleton does not have as bones; those channels were left out.");
            }
            if (morphChannels > 0) {
                warnings.push_back("Animation '" + animationName + "' has " + std::to_string(morphChannels) +
                                   " morph target weight channel(s), which are not supported; they were left out.");
            }
            return clip;
        }

        [[nodiscard]]
        std::size_t AnimationCount() const {
            return ArraySize("animations");
        }

        // --- Inspection --------------------------------------------------------------------------

        [[nodiscard]]
        Frost::GltfModelInfo Inspect() {
            Frost::GltfModelInfo info;

            VisitMeshNodes([&](const std::int64_t, const Value&, const std::int64_t, const glm::mat4&) { ++info.meshCount; });

            try {
                info.skinCount = GetSkins().size();
                const SkeletonBuild& build = GetSkeleton();
                info.boneCount = build.skeleton.bones.size();
                info.skinned = build.fromSkins;
                for (const Frost::Bone& bone : build.skeleton.bones) { info.boneNames.push_back(bone.name); }

                // The rigid parts (meshes without a skin), once per name, and where they would go.
                VisitMeshNodes([&](const std::int64_t nodeIndex, const Value& node, const std::int64_t meshIndex,
                                   const glm::mat4&) {
                    if (GetNodeSkin(node, nodeIndex).has_value()) { return; }
                    const std::string name = PartName(node, meshIndex);
                    if (std::any_of(info.parts.begin(), info.parts.end(),
                                    [&](const Frost::GltfPartInfo& part) { return part.name == name; })) {
                        return;
                    }

                    Frost::GltfPartInfo part;
                    part.name = name;
                    if (const std::optional<std::int32_t> parent = BoneAtOrAbove(nodeIndex)) {
                        part.parentBone = build.skeleton.bones[static_cast<std::size_t>(*parent)].name;
                    }
                    else {
                        part.suggestedBone = SuggestBone(name);
                    }
                    info.parts.push_back(std::move(part));
                });
            }
            catch (const std::runtime_error& e) {
                info.skeletonProblem = e.what();
                info.boneNames.clear();
                info.parts.clear();
            }

            const std::size_t animationCount = ArraySize("animations");
            for (std::size_t i = 0; i < animationCount; ++i) {
                Frost::GltfAnimationInfo animation;
                animation.name = AnimationDisplayName(i);
                try {
                    const Value& json = Lookup("animations", static_cast<std::int64_t>(i));
                    const Value* const name = json.Find("name");
                    animation.hasName = name != nullptr && name->IsString() && !name->AsString().empty();

                    for (const AnimationChannelInfo& channel : ReadAnimationChannels(json, i)) {
                        if (channel.path == ChannelPath::Weights) { continue; }
                        ++animation.channelCount;
                        const Sampler sampler = ReadSampler(json, channel.sampler, channel.path, i);
                        if (!sampler.times.empty()) { animation.duration = std::max(animation.duration, sampler.times.back()); }
                    }
                    if (animation.channelCount == 0) { animation.problem = "it does not move any node"; }
                }
                catch (const std::runtime_error& e) {
                    animation.problem = e.what();
                }
                info.animations.push_back(std::move(animation));
            }
            return info;
        }

    private:
        // --- Scene traversal -----------------------------------------------------------------------

        // Calls `visit(nodeIndex, node, meshIndex, world)` for every node of the default scene that
        // has a mesh, depth-first in file order; `world` is the node's world transform with the
        // scene scaled from meters to centimeters above its roots.
        template <typename Visit>
        void VisitMeshNodes(Visit&& visit) {
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
                    visit(pending.index, node, AsNonNegativeInteger(*mesh, "mesh"), world);
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

        // What a sub-mesh made from a node's mesh is called: the node's name, else the mesh's.
        [[nodiscard]]
        std::string PartName(const Value& node, const std::int64_t meshIndex) const {
            const std::string nodeName = GetName(node);
            return !nodeName.empty() ? nodeName : GetName(Lookup("meshes", meshIndex));
        }

        // A static-mesh import of a skinned mesh: posed by its skin with the joints where the file
        // puts them (world = joint world transform x inverse bind matrix, blended per vertex).
        // Primitives without skin weights are placed by the node, as unskinned meshes are.
        void AppendPosedSkinnedMesh(const std::int64_t meshIndex, const SkinInfo& skin, const std::string& name,
                                    const glm::mat4& nodeWorld, MeshData& out) {
            std::vector<glm::mat4> skinMatrices(skin.joints.size());
            for (std::size_t joint = 0; joint < skin.joints.size(); ++joint) {
                skinMatrices[joint] = GetWorldMeters()[static_cast<std::size_t>(skin.joints[joint])] * skin.inverseBind[joint];
            }
            const glm::mat4 metersToWorld = glm::scale(glm::mat4(1.0f), glm::vec3(Frost::kMeter));

            for (PrimitiveGeometry& geometry : ReadMesh(meshIndex, out, skin.joints.size())) {
                if (geometry.skin.empty()) { BakeTransform(geometry, nodeWorld); }
                else {
                    std::vector<Vertex> posed;
                    Frost::SkinVertices(geometry.vertices, geometry.skin, skinMatrices, posed);
                    geometry.vertices = std::move(posed);
                    BakeTransform(geometry, metersToWorld);
                }
                AppendGeometry(geometry, name, out);
            }
        }

        // --- Nodes -------------------------------------------------------------------------------

        // Every node, with its parent. Built once; throws if the hierarchy is not a forest.
        [[nodiscard]]
        const std::vector<NodeInfo>& GetNodes() {
            if (m_Nodes.has_value()) { return *m_Nodes; }

            const std::size_t nodeCount = ArraySize("nodes");
            std::vector<NodeInfo> nodes(nodeCount);
            std::unordered_set<std::string> names;

            for (std::size_t i = 0; i < nodeCount; ++i) {
                const auto index = static_cast<std::int64_t>(i);
                const Value& node = Lookup("nodes", index);
                NodeInfo& info = nodes[i];
                info.local = GetLocalTransform(node, index);
                info.pose = GetNodePose(node, info.local);

                std::string name = GetName(node);
                if (name.empty()) { name = "Node" + std::to_string(i); }
                while (!names.insert(name).second) { name += "_" + std::to_string(i); }
                info.name = std::move(name);

                if (const Value* const children = node.Find("children")) {
                    if (!children->IsArray()) { throw Error("Node ", i, " has a non-array 'children'."); }
                    for (const Value& childValue : children->AsArray()) {
                        const std::int64_t child = AsNonNegativeInteger(childValue, "children");
                        if (static_cast<std::size_t>(child) >= nodeCount) {
                            throw Error("Node ", i, " references non-existent child node ", child, ".");
                        }
                        info.children.push_back(child);
                    }
                }
            }

            for (std::size_t i = 0; i < nodeCount; ++i) {
                for (const std::int64_t child : nodes[i].children) {
                    NodeInfo& childInfo = nodes[static_cast<std::size_t>(child)];
                    if (childInfo.parent >= 0 || child == static_cast<std::int64_t>(i)) {
                        throw Error("Node ", child, " is reachable more than once; the node hierarchy is not a tree.");
                    }
                    childInfo.parent = static_cast<std::int64_t>(i);
                }
            }

            // Every node has at most one parent now; a cycle would be a loop of parents.
            for (std::size_t i = 0; i < nodeCount; ++i) {
                std::int64_t ancestor = nodes[i].parent;
                for (std::size_t steps = 0; ancestor >= 0; ++steps) {
                    if (steps > nodeCount) { throw Error("The node hierarchy contains a cycle (through node ", i, ")."); }
                    ancestor = nodes[static_cast<std::size_t>(ancestor)].parent;
                }
            }

            m_Nodes = std::move(nodes);
            return *m_Nodes;
        }

        // Every node's world transform in the file's units (meters), by node index.
        [[nodiscard]]
        const std::vector<glm::mat4>& GetWorldMeters() {
            if (m_WorldMeters.has_value()) { return *m_WorldMeters; }

            const std::vector<NodeInfo>& nodes = GetNodes();
            std::vector<glm::mat4> world(nodes.size(), glm::mat4(1.0f));
            std::vector<std::int64_t> stack;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].parent < 0) { stack.push_back(static_cast<std::int64_t>(i)); }
            }
            while (!stack.empty()) {
                const auto index = static_cast<std::size_t>(stack.back());
                stack.pop_back();

                const NodeInfo& node = nodes[index];
                world[index] = node.parent >= 0 ? world[static_cast<std::size_t>(node.parent)] * node.local : node.local;
                stack.insert(stack.end(), node.children.begin(), node.children.end());
            }

            m_WorldMeters = std::move(world);
            return *m_WorldMeters;
        }

        // A node's world transform with every length in centimeters (what bones use).
        [[nodiscard]]
        glm::mat4 WorldCentimeters(const std::int64_t node) {
            return ToCentimeters(GetWorldMeters()[static_cast<std::size_t>(node)]);
        }

        // A node's local transform as a bone pose: its translation/rotation/scale properties
        // directly (the translation in centimeters), or its matrix decomposed.
        [[nodiscard]]
        static Frost::BoneTransform GetNodePose(const Value& node, const glm::mat4& local) {
            if (node.Find("matrix") != nullptr) { return Frost::BoneTransform::FromMatrix(ToCentimeters(local)); }

            std::array<float, 3> translation = {0.0f, 0.0f, 0.0f};
            std::array<float, 4> rotation = {0.0f, 0.0f, 0.0f, 1.0f};
            std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
            static_cast<void>(GetNumberArray(node, "translation", translation));
            static_cast<void>(GetNumberArray(node, "rotation", rotation));
            static_cast<void>(GetNumberArray(node, "scale", scale));

            Frost::BoneTransform pose;
            pose.translation = glm::vec3(translation[0], translation[1], translation[2]) * Frost::kMeter;
            pose.rotation = NormalizedRotation(rotation[0], rotation[1], rotation[2], rotation[3]);
            pose.scale = glm::vec3(scale[0], scale[1], scale[2]);
            return pose;
        }

        // A transform in meters (S * m * S^-1 with S the meters-to-centimeters scale): the
        // translation scaled, the linear part as it is.
        [[nodiscard]]
        static glm::mat4 ToCentimeters(glm::mat4 matrix) noexcept {
            matrix[3] = glm::vec4(glm::vec3(matrix[3]) * Frost::kMeter, matrix[3].w);
            return matrix;
        }

        [[nodiscard]]
        static bool NearlyEqual(const glm::mat4& a, const glm::mat4& b) noexcept {
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    const float scale = std::max({1.0f, std::abs(a[column][row]), std::abs(b[column][row])});
                    if (std::abs(a[column][row] - b[column][row]) > 1.0e-3f * scale) { return false; }
                }
            }
            return true;
        }

        // 'a', 'b' and 'c' (at most eight, then "and n more").
        [[nodiscard]]
        static std::string JoinNames(const std::vector<std::string>& names) {
            constexpr std::size_t kShown = 8;
            std::string text;
            for (std::size_t i = 0; i < names.size() && i < kShown; ++i) {
                if (i > 0) { text += i + 1 == std::min(names.size(), kShown) && names.size() <= kShown ? " and " : ", "; }
                text += "'" + names[i] + "'";
            }
            if (names.size() > kShown) { text += " and " + std::to_string(names.size() - kShown) + " more"; }
            return text;
        }

        // --- Skins -------------------------------------------------------------------------------

        // Every skin, validated. Built once.
        [[nodiscard]]
        const std::vector<SkinInfo>& GetSkins() {
            if (m_Skins.has_value()) { return *m_Skins; }

            const std::size_t nodeCount = GetNodes().size();
            std::vector<SkinInfo> skins(ArraySize("skins"));
            for (std::size_t i = 0; i < skins.size(); ++i) {
                const Value& skin = Lookup("skins", static_cast<std::int64_t>(i));
                SkinInfo& info = skins[i];

                const Value* const joints = skin.Find("joints");
                if (joints == nullptr || !joints->IsArray() || joints->AsArray().empty()) {
                    throw Error("Skin ", i, " has no joints.");
                }
                std::unordered_set<std::int64_t> seen;
                for (const Value& joint : joints->AsArray()) {
                    const std::int64_t node = AsNonNegativeInteger(joint, "joints");
                    if (static_cast<std::size_t>(node) >= nodeCount) {
                        throw Error("Skin ", i, " references non-existent joint node ", node, ".");
                    }
                    if (!seen.insert(node).second) { throw Error("Skin ", i, " lists node ", node, " as a joint twice."); }
                    info.joints.push_back(node);
                }
                if (info.joints.size() > std::size_t{65536}) { throw Error("Skin ", i, " has too many joints."); }

                info.inverseBind.assign(info.joints.size(), glm::mat4(1.0f));
                if (const Value* const matrices = skin.Find("inverseBindMatrices")) {
                    const Accessor accessor = ResolveAccessor(AsNonNegativeInteger(*matrices, "inverseBindMatrices"));
                    if (accessor.componentCount != 16 || accessor.componentType != ComponentType::Float) {
                        throw Error("Skin ", i, " has inverse bind matrices that are not MAT4 floats.");
                    }
                    if (accessor.count < info.joints.size()) {
                        throw Error("Skin ", i, " has fewer inverse bind matrices than joints.");
                    }
                    const std::vector<float> values = ReadFloats(accessor);
                    for (std::size_t joint = 0; joint < info.joints.size(); ++joint) {
                        info.inverseBind[joint] = glm::make_mat4(values.data() + joint * 16);
                    }
                }
            }

            m_Skins = std::move(skins);
            return *m_Skins;
        }

        // The skin a node's mesh is bound to, if it has one.
        [[nodiscard]]
        std::optional<std::size_t> GetNodeSkin(const Value& node, const std::int64_t nodeIndex) const {
            const Value* const skin = node.Find("skin");
            if (skin == nullptr) { return std::nullopt; }

            const std::int64_t index = AsNonNegativeInteger(*skin, "skin");
            if (static_cast<std::size_t>(index) >= ArraySize("skins")) {
                throw Error("Node ", nodeIndex, " references non-existent skin ", index, ".");
            }
            return static_cast<std::size_t>(index);
        }

        // --- Skeleton ----------------------------------------------------------------------------

        // The file's skeleton. With skins: every joint of every skin and every node an animation
        // moves, plus every node above those (so the bones' transforms include whatever the file
        // places the skeleton under, e.g. an "Armature" node). Without: every node of the default
        // scene, so a model whose parts are animated as a node hierarchy can be imported as a
        // skeletal mesh too. Bones are ordered depth-first (scene roots first), which puts every
        // parent before its children.
        [[nodiscard]]
        const SkeletonBuild& GetSkeleton() {
            if (m_Skeleton.has_value()) { return *m_Skeleton; }

            const std::vector<NodeInfo>& nodes = GetNodes();
            const std::vector<SkinInfo>& skins = GetSkins();

            SkeletonBuild build;
            build.fromSkins = !skins.empty();
            build.nodeToBone.assign(nodes.size(), -1);

            // Roots to walk from: the scene's, then any other top-level node (a joint need not be
            // in the scene).
            std::vector<std::int64_t> roots = GetRootNodes();
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].parent < 0 && std::find(roots.begin(), roots.end(), static_cast<std::int64_t>(i)) == roots.end()) {
                    roots.push_back(static_cast<std::int64_t>(i));
                }
            }

            std::vector<bool> isBone(nodes.size(), false);
            if (build.fromSkins) {
                const auto markWithAncestors = [&](const std::int64_t first) {
                    for (std::int64_t node = first; node >= 0 && !isBone[static_cast<std::size_t>(node)];
                         node = nodes[static_cast<std::size_t>(node)].parent) {
                        isBone[static_cast<std::size_t>(node)] = true;
                    }
                };

                for (const SkinInfo& skin : skins) {
                    for (const std::int64_t joint : skin.joints) { markWithAncestors(joint); }
                }

                // Nodes an animation moves (a prop under a hand, a weapon's magazine) are bones too,
                // so their animation is not lost. A malformed animation is skipped here; importing
                // it reports the problem.
                for (std::size_t animation = 0; animation < AnimationCount(); ++animation) {
                    try {
                        const Value& json = Lookup("animations", static_cast<std::int64_t>(animation));
                        for (const AnimationChannelInfo& channel : ReadAnimationChannels(json, animation)) {
                            if (channel.path != ChannelPath::Weights) { markWithAncestors(channel.node); }
                        }
                    }
                    catch (const ImportError&) {
                    }
                }
            }
            else {
                // Only the default scene's nodes (GetRootNodes' part of `roots`).
                std::vector<std::int64_t> stack = GetRootNodes();
                while (!stack.empty()) {
                    const std::int64_t node = stack.back();
                    stack.pop_back();
                    isBone[static_cast<std::size_t>(node)] = true;
                    const std::vector<std::int64_t>& children = nodes[static_cast<std::size_t>(node)].children;
                    stack.insert(stack.end(), children.begin(), children.end());
                }
            }

            std::vector<std::int64_t> stack(roots.rbegin(), roots.rend());
            while (!stack.empty()) {
                const std::int64_t node = stack.back();
                stack.pop_back();
                const NodeInfo& info = nodes[static_cast<std::size_t>(node)];

                if (isBone[static_cast<std::size_t>(node)]) {
                    if (build.skeleton.bones.size() >= Frost::SkeletonData::kMaxBones) {
                        throw Error("The skeleton would have more than ", Frost::SkeletonData::kMaxBones, " bones.");
                    }
                    // Bones are closed upwards (every node above a bone is one), so the parent node
                    // is a bone already placed, or there is none.
                    const std::int32_t parent = info.parent >= 0 ? build.nodeToBone[static_cast<std::size_t>(info.parent)] : -1;
                    build.nodeToBone[static_cast<std::size_t>(node)] = static_cast<std::int32_t>(build.skeleton.bones.size());
                    build.boneToNode.push_back(node);
                    build.skeleton.bones.push_back(Frost::Bone{.name = info.name, .parent = parent, .restPose = info.pose});
                }

                stack.insert(stack.end(), info.children.rbegin(), info.children.rend());
            }

            if (build.skeleton.bones.empty()) { throw Error("File has no nodes to make a skeleton of."); }
            if (const std::optional<std::string> problem = build.skeleton.Validate()) {
                throw Error("The skeleton cannot be built: ", *problem, ".");
            }

            m_Skeleton = std::move(build);
            return *m_Skeleton;
        }

        // The bone of the file's skeleton at or nearest above a node, if any.
        [[nodiscard]]
        std::optional<std::int32_t> BoneAtOrAbove(std::int64_t node) {
            const SkeletonBuild& build = GetSkeleton();
            while (node >= 0) {
                if (const std::int32_t bone = build.nodeToBone[static_cast<std::size_t>(node)]; bone >= 0) { return bone; }
                node = GetNodes()[static_cast<std::size_t>(node)].parent;
            }
            return std::nullopt;
        }

        // Per bone of the file's skeleton, whether some skinned vertex is weighted to it. Read from
        // the skins' JOINTS/WEIGHTS attributes alone (nothing else of the meshes is decoded).
        [[nodiscard]]
        const std::vector<bool>& GetDeformingBones() {
            if (m_DeformingBones.has_value()) { return *m_DeformingBones; }

            const SkeletonBuild& build = GetSkeleton();
            std::vector<bool> deforming(build.skeleton.bones.size(), false);
            VisitMeshNodes([&](const std::int64_t nodeIndex, const Value& node, const std::int64_t meshIndex, const glm::mat4&) {
                const std::optional<std::size_t> skinIndex = GetNodeSkin(node, nodeIndex);
                if (!skinIndex.has_value()) { return; }
                const SkinInfo& skin = GetSkins().at(*skinIndex);

                const Value* const primitives = Lookup("meshes", meshIndex).Find("primitives");
                if (primitives == nullptr || !primitives->IsArray()) { return; }
                for (const Value& primitive : primitives->AsArray()) {
                    const Value* const attributes = primitive.Find("attributes");
                    const Value* const position = attributes != nullptr ? attributes->Find("POSITION") : nullptr;
                    if (position == nullptr) { continue; }

                    const std::size_t vertexCount = ResolveAccessor(AsNonNegativeInteger(*position, "POSITION")).count;
                    for (const SkinWeights& weights : ReadSkinWeights(*attributes, vertexCount, skin.joints.size())) {
                        for (std::size_t i = 0; i < SkinWeights::kMaxInfluences; ++i) {
                            if (weights.weights[i] > 0.0f) {
                                const std::int64_t joint = skin.joints[weights.bones[i]];
                                deforming[static_cast<std::size_t>(build.nodeToBone[static_cast<std::size_t>(joint)])] = true;
                            }
                        }
                    }
                }
            });

            m_DeformingBones = std::move(deforming);
            return *m_DeformingBones;
        }

        // The bone of the file's skeleton a loose part is attached to by default (see
        // GltfAttachmentOptions): of the bones that do something (skin joints and animated nodes;
        // not, say, the "Armature" node the rig sits under), the one whose name shares the most
        // words with the part's, preferring bones that deform nothing (they exist to carry parts),
        // then the first; else the skeleton's root joint (the first bone that is a joint of a skin).
        // Empty if the file has no skins.
        [[nodiscard]]
        std::string SuggestBone(const std::string& partName) {
            const SkeletonBuild& build = GetSkeleton();
            if (!build.fromSkins) { return {}; }

            const std::vector<std::string> partWords = NameWords(partName);
            const std::vector<bool>& deforming = GetDeformingBones();

            std::unordered_set<std::int64_t> joints;
            for (const SkinInfo& skin : GetSkins()) { joints.insert(skin.joints.begin(), skin.joints.end()); }
            std::unordered_set<std::int64_t> active = joints;
            for (std::size_t animation = 0; animation < AnimationCount(); ++animation) {
                try {
                    for (const AnimationChannelInfo& channel :
                         ReadAnimationChannels(Lookup("animations", static_cast<std::int64_t>(animation)), animation)) {
                        if (channel.path != ChannelPath::Weights) { active.insert(channel.node); }
                    }
                }
                catch (const ImportError&) {
                }
            }

            std::optional<std::size_t> best;
            std::pair<std::size_t, bool> bestScore{0, false};  // (shared words, deforms nothing)
            for (std::size_t bone = 0; bone < build.skeleton.bones.size(); ++bone) {
                if (!active.contains(build.boneToNode[bone])) { continue; }

                std::size_t shared = 0;
                for (const std::string& word : NameWords(build.skeleton.bones[bone].name)) {
                    shared += std::count(partWords.begin(), partWords.end(), word) > 0 ? std::size_t{1} : std::size_t{0};
                }
                const std::pair<std::size_t, bool> score{shared, !deforming[bone]};
                if (shared > 0 && (!best.has_value() || score > bestScore)) {
                    best = bone;
                    bestScore = score;
                }
            }
            if (best.has_value()) { return build.skeleton.bones[*best].name; }

            for (std::size_t bone = 0; bone < build.boneToNode.size(); ++bone) {
                if (joints.contains(build.boneToNode[bone])) { return build.skeleton.bones[bone].name; }
            }
            return {};
        }

        // The inverse bind matrix (in centimeters) a skeletal mesh uses for one of the file's bones:
        // the first skin's that has the bone as a joint, else the inverse of the bone's world
        // transform in the file (so whatever is bound to it stays where the file has it).
        [[nodiscard]]
        glm::mat4 BoneInverseBind(const std::int32_t bone) {
            const std::int64_t node = GetSkeleton().boneToNode[static_cast<std::size_t>(bone)];
            for (const SkinInfo& skin : GetSkins()) {
                for (std::size_t joint = 0; joint < skin.joints.size(); ++joint) {
                    if (skin.joints[joint] == node) { return ToCentimeters(skin.inverseBind[joint]); }
                }
            }

            const glm::mat4 world = WorldCentimeters(node);
            if (glm::determinant(glm::mat3(world)) == 0.0f) {
                throw Error("Bone '", GetNodes()[static_cast<std::size_t>(node)].name,
                            "' is scaled to nothing in the file, so what is bound to it cannot be placed.");
            }
            return glm::inverse(world);
        }

        // --- Animation channels ------------------------------------------------------------------

        // An animation's channels that target a node (channels without a target node, or with a
        // property path this importer does not know, such as an extension's, are skipped).
        [[nodiscard]]
        std::vector<AnimationChannelInfo> ReadAnimationChannels(const Value& animation, const std::size_t index) {
            const Value* const channels = animation.Find("channels");
            if (channels == nullptr || !channels->IsArray()) { throw Error("Animation ", index, " has no 'channels' array."); }

            const std::size_t nodeCount = GetNodes().size();
            std::vector<AnimationChannelInfo> result;
            for (const Value& channel : channels->AsArray()) {
                const Value* const target = channel.Find("target");
                if (target == nullptr || !target->IsObject()) { throw Error("Animation ", index, " has a channel without a target."); }

                const Value* const node = target->Find("node");
                const Value* const path = target->Find("path");
                if (node == nullptr || path == nullptr || !path->IsString()) { continue; }

                AnimationChannelInfo info;
                info.node = AsNonNegativeInteger(*node, "node");
                info.sampler = RequireInteger(channel, "sampler");
                if (static_cast<std::size_t>(info.node) >= nodeCount) {
                    throw Error("Animation ", index, " animates non-existent node ", info.node, ".");
                }

                const std::string& pathName = path->AsString();
                if (pathName == "translation") { info.path = ChannelPath::Translation; }
                else if (pathName == "rotation") { info.path = ChannelPath::Rotation; }
                else if (pathName == "scale") { info.path = ChannelPath::Scale; }
                else if (pathName == "weights") { info.path = ChannelPath::Weights; }
                else { continue; }

                result.push_back(info);
            }
            return result;
        }

        [[nodiscard]]
        Sampler ReadSampler(const Value& animation, const std::int64_t samplerIndex, const ChannelPath path,
                            const std::size_t animationIndex) const {
            const Value* const samplers = animation.Find("samplers");
            const Value* const sampler = samplers != nullptr ? samplers->At(static_cast<std::size_t>(samplerIndex)) : nullptr;
            if (sampler == nullptr || !sampler->IsObject()) {
                throw Error("Animation ", animationIndex, " references non-existent sampler ", samplerIndex, ".");
            }

            Sampler result;
            result.componentCount = path == ChannelPath::Rotation ? 4 : 3;

            if (const Value* const interpolation = sampler->Find("interpolation")) {
                const std::string name = interpolation->IsString() ? interpolation->AsString() : std::string();
                if (name == "LINEAR") { result.interpolation = Frost::AnimationInterpolation::Linear; }
                else if (name == "STEP") { result.interpolation = Frost::AnimationInterpolation::Step; }
                else if (name == "CUBICSPLINE") { result.interpolation = Frost::AnimationInterpolation::CubicSpline; }
                else { throw Error("Animation ", animationIndex, " has a sampler with unknown interpolation '", name, "'."); }
            }

            const Accessor input = ResolveAccessor(RequireInteger(*sampler, "input"));
            if (input.componentCount != 1 || input.componentType != ComponentType::Float) {
                throw Error("Animation ", animationIndex, " has keyframe times that are not SCALAR floats.");
            }
            result.times = ReadFloats(input);
            for (std::size_t i = 0; i < result.times.size(); ++i) {
                if (!std::isfinite(result.times[i]) || (i > 0 && result.times[i] < result.times[i - 1])) {
                    throw Error("Animation ", animationIndex, " has keyframe times that are not in increasing order.");
                }
            }

            // Weights channels are not imported, so their values are not needed (or checked).
            if (path == ChannelPath::Weights) { return result; }

            const Accessor output = ResolveAccessor(RequireInteger(*sampler, "output"));
            if (output.componentCount != result.componentCount) {
                throw Error("Animation ", animationIndex, " has ", path == ChannelPath::Rotation ? "rotation" : "translation/scale",
                            " keyframes that are not VEC", result.componentCount, ".");
            }
            const std::size_t valuesPerKey = result.interpolation == Frost::AnimationInterpolation::CubicSpline ? 3 : 1;
            if (output.count != input.count * valuesPerKey) {
                throw Error("Animation ", animationIndex, " has a sampler with ", output.count, " values for ", input.count,
                            " keyframes.");
            }
            result.values = ReadFloats(output);
            for (const float value : result.values) {
                if (!std::isfinite(value)) { throw Error("Animation ", animationIndex, " has non-finite keyframe values."); }
            }
            return result;
        }

        [[nodiscard]]
        static Frost::VectorChannel ToVectorChannel(const Sampler& sampler, const float scale) {
            Frost::VectorChannel channel;
            channel.interpolation = sampler.interpolation;
            channel.times = sampler.times;
            channel.values.reserve(sampler.values.size() / 3);
            for (std::size_t i = 0; i + 2 < sampler.values.size(); i += 3) {
                channel.values.emplace_back(sampler.values[i] * scale, sampler.values[i + 1] * scale, sampler.values[i + 2] * scale);
            }
            return channel;
        }

        [[nodiscard]]
        static Frost::RotationChannel ToRotationChannel(const Sampler& sampler) {
            Frost::RotationChannel channel;
            channel.interpolation = sampler.interpolation;
            channel.times = sampler.times;
            channel.values.reserve(sampler.values.size() / 4);
            const bool cubic = sampler.interpolation == Frost::AnimationInterpolation::CubicSpline;
            for (std::size_t i = 0; i + 3 < sampler.values.size(); i += 4) {
                const glm::quat value =
                        Frost::MakeQuaternion(sampler.values[i], sampler.values[i + 1], sampler.values[i + 2], sampler.values[i + 3]);
                // Key values are rotations (normalized, as exporters' may be slightly off); a cubic
                // spline's tangents are not, and sampling normalizes what the spline gives.
                const bool isTangent = cubic && (i / 4) % 3 != 1;
                channel.values.push_back(isTangent ? value : NormalizedRotation(value.x, value.y, value.z, value.w));
            }
            return channel;
        }

        // An animation's name: its own, or "Animation <index>" if it has none.
        [[nodiscard]]
        std::string AnimationDisplayName(const std::size_t index) const {
            const Value& animation = Lookup("animations", static_cast<std::int64_t>(index));
            const std::string name = GetName(animation);
            return name.empty() ? "Animation " + std::to_string(index) : name;
        }

        // Forgets which glTF materials/textures became which MeshData entries: each import builds
        // its own MeshData.
        void ResetCaches() {
            m_TextureCache.clear();
            m_MaterialCache.clear();
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

            const glm::quat quaternion = NormalizedRotation(rotation[0], rotation[1], rotation[2], rotation[3]);

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
            else if (typeName == "MAT4" && accessor.componentType == ComponentType::Float) {
                // Skins' inverse bind matrices. With 4-byte components a MAT4's columns need no
                // padding, so it reads as 16 consecutive floats (column-major, like glm).
                accessor.componentCount = 16;
            }
            else {
                // MAT2/MAT3 (and small-component MAT4s) never carry data the importer uses, and their
                // byte-padded column layout would need its own decode path; anything else is invalid.
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

        // One triangle primitive, read into the engine's vertex layout in its mesh's own space (in
        // the file's units, meters): a triangle list with normals (flat ones generated if the file
        // has none) and tangents (derived if it has none but has UVs), optionally with its skin
        // weights. Transforms are applied separately (BakeTransform).
        struct PrimitiveGeometry {
            std::vector<Vertex> vertices;
            std::vector<std::uint32_t> indices;  // Triangles, into `vertices`.
            // Per vertex, when skin weights were requested and the primitive has them: influences by
            // index into its skin's `joints`, at most four, weights summing to 1 (or all 0).
            std::vector<SkinWeights> skin;
            std::int32_t materialIndex = -1;  // Into the output's materials.
        };

        // Reads every primitive of mesh `meshIndex` (materials into `out`), skipping those that are
        // not triangle geometry. With `skinJointCount` set, skin weights are read too, and their
        // joint indices checked against it.
        [[nodiscard]]
        std::vector<PrimitiveGeometry> ReadMesh(const std::int64_t meshIndex, MeshData& out,
                                                const std::optional<std::size_t> skinJointCount) {
            const Value& mesh = Lookup("meshes", meshIndex);

            const Value* const primitives = mesh.Find("primitives");
            if (primitives == nullptr || !primitives->IsArray()) {
                throw Error("Mesh ", meshIndex, " has no 'primitives' array.");
            }

            std::vector<PrimitiveGeometry> result;
            for (std::size_t i = 0; i < primitives->AsArray().size(); ++i) {
                const Value& primitive = primitives->AsArray()[i];
                if (!primitive.IsObject()) { throw Error("Mesh ", meshIndex, " primitive ", i, " is not an object."); }

                if (std::optional<PrimitiveGeometry> geometry = ReadPrimitive(primitive, out, skinJointCount)) {
                    result.push_back(std::move(*geometry));
                }
            }
            return result;
        }

        [[nodiscard]]
        std::optional<PrimitiveGeometry> ReadPrimitive(const Value& primitive, MeshData& out,
                                                       const std::optional<std::size_t> skinJointCount) {
            const auto mode = static_cast<PrimitiveMode>(GetInteger(primitive, "mode", 4));
            if (mode != PrimitiveMode::Triangles && mode != PrimitiveMode::TriangleStrip &&
                mode != PrimitiveMode::TriangleFan) {
                return std::nullopt;  // Points and lines are not triangle geometry; see the header comment.
            }

            const Value* const attributes = primitive.Find("attributes");
            if (attributes == nullptr || !attributes->IsObject()) {
                throw Error("Primitive has no 'attributes' object.");
            }

            // Without positions there is nothing to draw (the specification says to skip the
            // primitive).
            const Value* const positionIndex = attributes->Find("POSITION");
            if (positionIndex == nullptr) { return std::nullopt; }

            const Accessor positionAccessor = ResolveAccessor(AsNonNegativeInteger(*positionIndex, "POSITION"));
            if (positionAccessor.componentCount != 3) { throw Error("POSITION attribute is not a VEC3."); }

            const std::size_t vertexCount = positionAccessor.count;
            if (vertexCount == 0) { return std::nullopt; }

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

            std::vector<SkinWeights> skin;
            if (skinJointCount.has_value()) { skin = ReadSkinWeights(*attributes, vertexCount, *skinJointCount); }

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
            if (indices.empty()) { return std::nullopt; }

            PrimitiveGeometry geometry;
            geometry.materialIndex = ResolveMaterial(primitive, out);

            // Assemble local-space vertices in the engine's layout.
            std::vector<Vertex>& vertices = geometry.vertices;
            vertices.resize(vertexCount);
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
            // triangle corner. De-index the primitive (and its skin) and assign each face's
            // geometric normal.
            if (!hasNormals) {
                std::vector<Vertex> flatVertices;
                std::vector<SkinWeights> flatSkin;
                flatVertices.reserve(indices.size());
                if (!skin.empty()) { flatSkin.reserve(indices.size()); }

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

                    if (!skin.empty()) {
                        for (std::size_t corner = 0; corner < 3; ++corner) { flatSkin.push_back(skin[indices[i + corner]]); }
                    }
                }

                vertices = std::move(flatVertices);
                skin = std::move(flatSkin);
                std::iota(indices.begin(), indices.end(), 0u);
            }

            // Normal mapping needs a tangent frame. Files rarely carry TANGENT; derive it from the
            // UV layout when they do not (and there is a UV layout to derive it from).
            if (!hasTangents && hasUvs) { GenerateTangents(vertices, indices); }

            geometry.indices = std::move(indices);
            geometry.skin = std::move(skin);
            return geometry;
        }

        // A primitive's skin weights, from its JOINTS_n/WEIGHTS_n attribute pairs (n = 0, 1, ...):
        // per vertex the four strongest influences, renormalized to sum to 1. Vertices without any
        // weight get none (all 0). Empty if the primitive has no JOINTS_0/WEIGHTS_0.
        [[nodiscard]]
        std::vector<SkinWeights> ReadSkinWeights(const Value& attributes, const std::size_t vertexCount,
                                                 const std::size_t jointCount) const {
            struct Influence {
                std::uint32_t joint = 0;
                float weight = 0.0f;
            };
            std::vector<std::vector<Influence>> influences(vertexCount);

            bool any = false;
            for (int set = 0;; ++set) {
                const std::string jointsName = "JOINTS_" + std::to_string(set);
                const std::string weightsName = "WEIGHTS_" + std::to_string(set);
                if (attributes.Find(jointsName) == nullptr || attributes.Find(weightsName) == nullptr) { break; }

                std::vector<float> joints;
                std::vector<float> weights;
                static_cast<void>(ReadAttribute(attributes, jointsName, {4}, vertexCount, joints));
                static_cast<void>(ReadAttribute(attributes, weightsName, {4}, vertexCount, weights));
                any = true;

                for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
                    for (std::size_t i = 0; i < 4; ++i) {
                        const float weight = weights[vertex * 4 + i];
                        if (!(weight > 0.0f)) { continue; }  // Zero, negative or NaN: no influence.

                        // Joint indices are unsigned byte/short components (read as exact floats).
                        const float joint = joints[vertex * 4 + i];
                        if (joint < 0.0f || joint >= static_cast<float>(jointCount) || std::floor(joint) != joint) {
                            throw Error("Attribute ", jointsName, " refers to joint ", joint, " of a skin with ",
                                        jointCount, " joints.");
                        }
                        influences[vertex].push_back(Influence{.joint = static_cast<std::uint32_t>(joint), .weight = weight});
                    }
                }
            }
            if (!any) { return {}; }

            std::vector<SkinWeights> skin(vertexCount);
            for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
                std::vector<Influence>& list = influences[vertex];

                // The same joint listed twice (possible across sets) counts once, with both weights.
                std::sort(list.begin(), list.end(), [](const Influence& a, const Influence& b) { return a.joint < b.joint; });
                std::vector<Influence> merged;
                for (const Influence& influence : list) {
                    if (!merged.empty() && merged.back().joint == influence.joint) { merged.back().weight += influence.weight; }
                    else { merged.push_back(influence); }
                }

                std::sort(merged.begin(), merged.end(),
                          [](const Influence& a, const Influence& b) { return a.weight > b.weight; });
                if (merged.size() > SkinWeights::kMaxInfluences) { merged.resize(SkinWeights::kMaxInfluences); }

                float total = 0.0f;
                for (const Influence& influence : merged) { total += influence.weight; }
                if (!(total > 0.0f) || !std::isfinite(total)) { continue; }

                for (std::size_t i = 0; i < merged.size(); ++i) {
                    skin[vertex].bones[i] = static_cast<std::uint16_t>(merged[i].joint);
                    skin[vertex].weights[i] = merged[i].weight / total;
                }
            }
            return skin;
        }

        // Bakes `matrix` into a primitive's vertices. Positions take the full matrix; normals take
        // the inverse-transpose of its linear part so non-uniform scale keeps them perpendicular
        // (falling back to the linear part itself if the matrix is singular, where no correct
        // normal exists anyway); tangents are surface directions and take the linear part.
        static void BakeTransform(PrimitiveGeometry& geometry, const glm::mat4& matrix) {
            const glm::mat3 linear(matrix);
            const float determinant = glm::determinant(linear);
            const glm::mat3 normalMatrix = determinant != 0.0f ? glm::inverseTranspose(linear) : linear;

            for (Vertex& vertex : geometry.vertices) {
                vertex.position = glm::vec3(matrix * glm::vec4(vertex.position, 1.0f));
                vertex.normal = SafeNormalize(normalMatrix * vertex.normal);
                vertex.tangent = glm::vec4(SafeNormalize(linear * glm::vec3(vertex.tangent)), vertex.tangent.w);
            }

            // A mirroring transform turns counter-clockwise triangles clockwise; swap two corners
            // of every triangle so front faces stay front faces under back-face culling. It also
            // flips the handedness of every tangent frame.
            if (determinant < 0.0f) {
                std::vector<std::uint32_t>& indices = geometry.indices;
                for (std::size_t i = 0; i + 2 < indices.size(); i += 3) { std::swap(indices[i + 1], indices[i + 2]); }
                for (Vertex& vertex : geometry.vertices) { vertex.tangent.w = -vertex.tangent.w; }
            }
        }

        // Appends a primitive to `out` as one sub-mesh. Its indices stay relative to its own first
        // vertex; the sub-mesh's vertexOffset rebases them at draw time.
        static void AppendGeometry(const PrimitiveGeometry& geometry, const std::string& name, MeshData& out) {
            if (out.vertices.size() + geometry.vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                throw Error("Model has too many vertices for a single mesh.");
            }
            if (out.indices.size() + geometry.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw Error("Model has too many indices for a single mesh.");
            }

            out.subMeshes.push_back(SubMesh{.firstIndex = static_cast<std::uint32_t>(out.indices.size()),
                                            .indexCount = static_cast<std::uint32_t>(geometry.indices.size()),
                                            .vertexOffset = static_cast<std::int32_t>(out.vertices.size()),
                                            .name = name,
                                            .materialIndex = geometry.materialIndex});

            out.vertices.insert(out.vertices.end(), geometry.vertices.begin(), geometry.vertices.end());
            out.indices.insert(out.indices.end(), geometry.indices.begin(), geometry.indices.end());
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
        // The scene graph, skins and skeleton, built on first use (see GetNodes etc.).
        std::optional<std::vector<NodeInfo>> m_Nodes;
        std::optional<std::vector<glm::mat4>> m_WorldMeters;
        std::optional<std::vector<SkinInfo>> m_Skins;
        std::optional<SkeletonBuild> m_Skeleton;
        std::optional<std::vector<bool>> m_DeformingBones;
    };

}  // namespace

namespace Frost {

    // The file's bytes and parsed JSON, and the Document reading them (which refers into both, so
    // they live together, at a fixed address).
    struct GltfModel::Impl {
        std::filesystem::path path;
        std::vector<std::byte> bytes;
        Value root;
        std::optional<Document> document;
        std::optional<GltfModelInfo> info;

        // Runs `function` on the document, reporting any failure as an ImportError naming the file.
        template <typename Function>
        auto Run(Function&& function) -> decltype(function(*document)) {
            try {
                return function(*document);
            }
            catch (const std::runtime_error& e) {  // ImportError, or a JSON parse error.
                throw ImportError("Failed to import glTF file '" + path.string() + "': " + e.what());
            }
        }
    };

    GltfModel::GltfModel(std::unique_ptr<Impl> impl) noexcept : m_Impl(std::move(impl)) {}

    GltfModel::~GltfModel() = default;

    GltfModel::GltfModel(GltfModel&&) noexcept = default;

    GltfModel& GltfModel::operator=(GltfModel&&) noexcept = default;

    GltfModel GltfModel::Open(const std::filesystem::path& path) {
        auto impl = std::make_unique<Impl>();
        impl->path = path;

        try {
            impl->bytes = ReadFile(path);
            const GltfContents contents = ParseContainer(impl->bytes);
            impl->root = Json::Parse(contents.json);

            if (!impl->root.IsObject()) { throw Error("glTF JSON root is not an object."); }

            impl->document.emplace(impl->root, contents.bin, path.parent_path());
            impl->document->CheckCompatibility();
        }
        catch (const std::runtime_error& e) {  // ImportError, or a JSON parse error.
            throw ImportError("Failed to import glTF file '" + path.string() + "': " + e.what());
        }

        return GltfModel(std::move(impl));
    }

    const std::filesystem::path& GltfModel::GetPath() const noexcept { return m_Impl->path; }

    const GltfModelInfo& GltfModel::GetInfo() {
        if (!m_Impl->info.has_value()) {
            m_Impl->info = m_Impl->Run([](Document& document) { return document.Inspect(); });
        }
        return *m_Impl->info;
    }

    MeshData GltfModel::ImportMesh() {
        return m_Impl->Run([](Document& document) {
            MeshData data;
            document.ImportStaticMesh(data);
            if (data.subMeshes.empty()) { throw Error("File contains no triangle geometry."); }
            return data;
        });
    }

    SkeletonData GltfModel::ImportSkeleton() {
        return m_Impl->Run([](Document& document) { return document.ImportSkeleton(); });
    }

    SkeletalMeshData GltfModel::ImportSkeletalMesh(const SkeletonData& skeleton, std::vector<std::string>& warnings,
                                                   const GltfAttachmentOptions& attachments,
                                                   std::vector<GltfSeparatePart>* const separateParts) {
        return m_Impl->Run([&](Document& document) {
            if (separateParts != nullptr) { separateParts->clear(); }
            return document.ImportSkeletalMesh(skeleton, warnings, attachments, separateParts);
        });
    }

    AnimationClipData GltfModel::ImportAnimation(const std::size_t index, const SkeletonData& skeleton,
                                                 std::vector<std::string>& warnings) {
        return m_Impl->Run([&](Document& document) {
            if (index >= document.AnimationCount()) { throw Error("The file has no animation ", index, "."); }
            return document.ImportAnimation(index, skeleton, warnings);
        });
    }

    MeshData GltfImporter::Import(const std::filesystem::path& path) { return GltfModel::Open(path).ImportMesh(); }

}  // namespace Frost
