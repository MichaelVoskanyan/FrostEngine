#include <Frost/Assets/AssetFile.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Units.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// Values are copied to and from the file with memcpy, which is only the file's little-endian
// layout on a little-endian machine. Every platform Frost targets is; a big-endian port would
// byte-swap in BinaryWriter::Write/BinaryReader::Read.
static_assert(std::endian::native == std::endian::little, "Frost asset files assume a little-endian host.");

namespace {

    constexpr std::array<char, 4> kMeshMagic = {'F', 'M', 'S', 'H'};
    constexpr std::array<char, 4> kTextureMagic = {'F', 'T', 'E', 'X'};
    constexpr std::array<char, 4> kSkeletonMagic = {'F', 'S', 'K', 'L'};
    constexpr std::array<char, 4> kSkeletalMeshMagic = {'F', 'S', 'K', 'M'};
    constexpr std::array<char, 4> kAnimationMagic = {'F', 'A', 'N', 'M'};
    constexpr std::array<char, 4> kSoundMagic = {'F', 'S', 'N', 'D'};

    [[nodiscard]]
    constexpr const std::array<char, 4>& MagicOf(const Frost::AssetType type) noexcept {
        switch (type) {
            case Frost::AssetType::Mesh: return kMeshMagic;
            case Frost::AssetType::Texture: return kTextureMagic;
            case Frost::AssetType::Skeleton: return kSkeletonMagic;
            case Frost::AssetType::SkeletalMesh: return kSkeletalMeshMagic;
            case Frost::AssetType::Animation: return kAnimationMagic;
            case Frost::AssetType::Sound: return kSoundMagic;
        }
        return kMeshMagic;
    }

    // Sanity limits for counts read from a file, so a corrupt count fails cleanly instead of
    // attempting a huge allocation. Generous for real content.
    constexpr std::uint32_t kMaxStringBytes = 64 * 1024;
    constexpr std::uint32_t kMaxTextureDimension = 16384;

    // Floats per vertex on disk: position 3, normal 3, uv 2, color 4, tangent 4.
    constexpr std::size_t kFloatsPerVertex = 16;

    // Bytes of the smallest possible record of each kind, for ReadCount's check that a declared
    // count fits in what is left of the file.
    constexpr std::size_t kMinBoneBytes = 4 + 4 + 10 * 4;       // Empty name, parent, 10 floats.
    constexpr std::size_t kSkinWeightBytes = 4 * 2 + 4 * 4;     // u16[4] bones, f32[4] weights.
    constexpr std::size_t kMinBindEntryBytes = 4 + 16 * 4;      // Empty name, mat4.
    constexpr std::size_t kMinTrackBytes = 4 + 3 * (1 + 4);     // Empty name, three empty channels.
    constexpr std::size_t kReferenceBytes = 8 + 8 + 4;          // Id, empty path.

    // At least the most a header can take (magic, version, id, two strings; as AssetFile::ReadHeader
    // reads), and a sound's properties.
    constexpr std::size_t kMaxHeaderBytes = 32 + 2 * (4 + kMaxStringBytes);
    constexpr std::size_t kSoundInfoBytes = 1 + 4 + 2 + 2 + 8 + 4;

    // memcpy, except that copying nothing is fine even from/to a null pointer (which an empty
    // vector's data() may be; memcpy with a null pointer is undefined even for zero bytes).
    void CopyBytes(void* const destination, const void* const source, const std::size_t size) {
        if (size != 0) { std::memcpy(destination, source, size); }
    }

    class BinaryWriter final {
    public:
        template <typename T>
        void Write(const T value) {
            static_assert(std::is_arithmetic_v<T>);
            const auto* const bytes = reinterpret_cast<const std::byte*>(&value);
            m_Bytes.insert(m_Bytes.end(), bytes, bytes + sizeof(T));
        }

        void WriteBytes(const void* const data, const std::size_t size) {
            const auto* const bytes = static_cast<const std::byte*>(data);
            m_Bytes.insert(m_Bytes.end(), bytes, bytes + size);
        }

        void WriteString(const std::string_view text) {
            Write(static_cast<std::uint32_t>(text.size()));
            WriteBytes(text.data(), text.size());
        }

        void WriteMagic(const std::array<char, 4>& magic) { WriteBytes(magic.data(), magic.size()); }

        [[nodiscard]]
        const std::vector<std::byte>& GetBytes() const noexcept {
            return m_Bytes;
        }

    private:
        std::vector<std::byte> m_Bytes;
    };

    class BinaryReader final {
    public:
        BinaryReader(const std::span<const std::byte> bytes, std::string description)
            : m_Bytes(bytes), m_Description(std::move(description)) {}

        template <typename T>
        [[nodiscard]]
        T Read() {
            static_assert(std::is_arithmetic_v<T>);
            T value{};
            std::memcpy(&value, Take(sizeof(T)).data(), sizeof(T));
            return value;
        }

        // A count of items of `itemSize` bytes each that must all still fit in the file.
        [[nodiscard]]
        std::uint32_t ReadCount(const std::size_t itemSize, const char* const what) {
            const auto count = Read<std::uint32_t>();
            if (itemSize > 0 && count > Remaining() / itemSize) {
                Fail(std::string("declares more ") + what + " than the file holds");
            }
            return count;
        }

        [[nodiscard]]
        std::span<const std::byte> ReadBytes(const std::size_t size) {
            return Take(size);
        }

        [[nodiscard]]
        std::string ReadString() {
            const auto size = Read<std::uint32_t>();
            if (size > kMaxStringBytes) { Fail("has an implausibly long string"); }

            const std::span<const std::byte> bytes = Take(size);
            return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }

        [[nodiscard]]
        std::array<char, 4> ReadMagic() {
            std::array<char, 4> magic{};
            std::memcpy(magic.data(), Take(magic.size()).data(), magic.size());
            return magic;
        }

        [[nodiscard]]
        std::size_t Remaining() const noexcept {
            return m_Bytes.size() - m_Offset;
        }

        // Malformed data.
        [[noreturn]]
        void Fail(const std::string& problem) const {
            throw Frost::ImportError(m_Description + " " + problem + " (corrupt or truncated file?).");
        }

        // A well-formed file that is not what was asked for (wrong type, newer version).
        [[noreturn]]
        void Reject(const std::string& problem) const {
            throw Frost::ImportError(m_Description + " " + problem + ".");
        }

    private:
        [[nodiscard]]
        std::span<const std::byte> Take(const std::size_t size) {
            if (size > Remaining()) { Fail("ends unexpectedly"); }
            const std::span<const std::byte> bytes = m_Bytes.subspan(m_Offset, size);
            m_Offset += size;
            return bytes;
        }

        std::span<const std::byte> m_Bytes;
        std::size_t m_Offset = 0;
        std::string m_Description;
    };

    [[nodiscard]]
    std::vector<std::byte> ReadWholeFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) { throw Frost::ImportError("Could not open asset '" + Frost::PathToUtf8(path) + "'."); }

        std::vector<char> chars{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (file.bad()) { throw Frost::ImportError("Could not read asset '" + Frost::PathToUtf8(path) + "'."); }

        std::vector<std::byte> bytes(chars.size());
        CopyBytes(bytes.data(), chars.data(), chars.size());
        return bytes;
    }

    void WriteFileAtomically(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
        std::error_code error;
        if (!path.parent_path().empty()) { std::filesystem::create_directories(path.parent_path(), error); }
        if (error) {
            throw Frost::ImportError("Could not create directory '" + Frost::PathToUtf8(path.parent_path()) +
                                     "': " + error.message());
        }

        // Written next to the target and renamed over it, so a failure mid-write never leaves a
        // truncated asset behind (or destroys the previous version).
        std::filesystem::path temporary = path;
        temporary += ".tmp";

        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.close();

            if (!file) {
                std::filesystem::remove(temporary, error);
                throw Frost::ImportError("Could not write asset '" + Frost::PathToUtf8(path) + "'.");
            }
        }

        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw Frost::ImportError("Could not replace asset '" + Frost::PathToUtf8(path) + "'.");
        }
    }

    // Version 1 stored paths relative to the asset file's own directory.
    [[nodiscard]]
    std::filesystem::path AssetDirectory(const std::filesystem::path& assetPath) {
        return Frost::AssetManager::NormalizePath(assetPath).parent_path();
    }

    // Paths are stored relative to the project directory when inside it (see MakeProjectPath).
    void WritePath(BinaryWriter& writer, const std::filesystem::path& path, const std::filesystem::path& projectDirectory) {
        const std::filesystem::path normalized = path.empty() ? path : Frost::AssetManager::NormalizePath(path);
        const std::filesystem::path project =
                projectDirectory.empty() ? projectDirectory : Frost::AssetManager::NormalizePath(projectDirectory);
        writer.WriteString(Frost::GenericPathToUtf8(Frost::MakeProjectPath(normalized, project)));
    }

    [[nodiscard]]
    std::filesystem::path ReadPath(BinaryReader& reader, const std::filesystem::path& baseDirectory, const bool legacy) {
        const std::filesystem::path stored = Frost::PathFromUtf8(reader.ReadString());
        if (stored.empty()) { return {}; }
        const std::filesystem::path resolved = legacy ? Frost::ResolvePortablePath(stored, baseDirectory)
                                                      : Frost::ResolveProjectPath(stored, baseDirectory);
        return resolved.is_absolute() ? Frost::AssetManager::NormalizePath(resolved) : resolved;
    }

    void WriteId(BinaryWriter& writer, const Frost::AssetId& id) {
        writer.Write(id.high);
        writer.Write(id.low);
    }

    [[nodiscard]]
    Frost::AssetId ReadId(BinaryReader& reader) {
        Frost::AssetId id;
        id.high = reader.Read<std::uint64_t>();
        id.low = reader.Read<std::uint64_t>();
        return id;
    }

    // The current version of `type` (the header's type/version are not used).
    void WriteHeader(BinaryWriter& writer, const Frost::AssetType type, const std::filesystem::path& assetPath,
                     const Frost::AssetHeader& header, const std::filesystem::path& projectDirectory) {
        if (!header.id.IsValid()) {
            throw Frost::ImportError("Asset '" + Frost::PathToUtf8(assetPath) + "' cannot be written without an id.");
        }

        writer.WriteMagic(MagicOf(type));
        writer.Write(Frost::AssetFile::GetCurrentVersion(type));
        WriteId(writer, header.id);
        WritePath(writer, header.sourcePath, projectDirectory);
        writer.WriteString(header.sourceItem);
    }

    // Reads the header of a file expected to be of one type (any version this build reads).
    [[nodiscard]]
    Frost::AssetHeader ReadHeader(BinaryReader& reader, const Frost::AssetType type, const std::filesystem::path& assetPath,
                                  const std::filesystem::path& projectDirectory) {
        if (reader.ReadMagic() != MagicOf(type)) {
            reader.Reject(std::string("is not a Frost ") + Frost::GetAssetTypeName(type) + " asset");
        }

        const std::uint32_t supportedVersion = Frost::AssetFile::GetCurrentVersion(type);

        Frost::AssetHeader header;
        header.type = type;
        header.version = reader.Read<std::uint32_t>();
        if (header.version == 0) { reader.Fail("has an invalid format version"); }
        if (header.version > supportedVersion) {
            reader.Reject("has format version " + std::to_string(header.version) +
                          ", which this build does not support (" + std::to_string(supportedVersion) +
                          " or older); it was saved by a newer version of Frost");
        }

        // Version-1 meshes and textures had no id and no source item, and stored the source path
        // relative to the asset itself.
        const bool hasId = Frost::AssetFile::HasId(type, header.version);
        if (hasId) { header.id = ReadId(reader); }
        header.sourcePath = ReadPath(reader, hasId ? projectDirectory : AssetDirectory(assetPath), !hasId);
        if (hasId) { header.sourceItem = reader.ReadString(); }
        return header;
    }

    void WriteReference(BinaryWriter& writer, const Frost::AssetReference& reference,
                        const std::filesystem::path& projectDirectory) {
        WriteId(writer, reference.id);
        WritePath(writer, reference.path, projectDirectory);
    }

    [[nodiscard]]
    Frost::AssetReference ReadReference(BinaryReader& reader, const std::filesystem::path& projectDirectory) {
        Frost::AssetReference reference;
        reference.id = ReadId(reader);
        reference.path = ReadPath(reader, projectDirectory, false);
        return reference;
    }

    void WriteTextureSettings(BinaryWriter& writer, const Frost::TextureSettings& settings) {
        writer.Write(static_cast<std::uint8_t>(settings.magFilter));
        writer.Write(static_cast<std::uint8_t>(settings.minFilter));
        writer.Write(static_cast<std::uint8_t>(settings.mipmapMode));
        writer.Write(static_cast<std::uint8_t>(settings.wrapU));
        writer.Write(static_cast<std::uint8_t>(settings.wrapV));
        writer.Write(static_cast<std::uint8_t>(settings.srgb ? 1 : 0));
    }

    [[nodiscard]]
    Frost::TextureSettings ReadTextureSettings(BinaryReader& reader) {
        const auto enumValue = [&](const std::uint8_t max) {
            const auto value = reader.Read<std::uint8_t>();
            if (value > max) { reader.Fail("has an invalid texture sampler setting"); }
            return value;
        };

        Frost::TextureSettings settings;
        settings.magFilter = static_cast<Frost::TextureFilter>(enumValue(1));
        settings.minFilter = static_cast<Frost::TextureFilter>(enumValue(1));
        settings.mipmapMode = static_cast<Frost::TextureMipmapMode>(enumValue(2));
        settings.wrapU = static_cast<Frost::TextureWrap>(enumValue(2));
        settings.wrapV = static_cast<Frost::TextureWrap>(enumValue(2));
        settings.srgb = enumValue(1) != 0;
        return settings;
    }

    void WriteImage(BinaryWriter& writer, const Frost::ImageData& image) {
        writer.Write(image.width);
        writer.Write(image.height);
        writer.WriteBytes(image.pixels.data(), image.pixels.size());
    }

    [[nodiscard]]
    Frost::ImageData ReadImage(BinaryReader& reader) {
        Frost::ImageData image;
        image.width = reader.Read<std::uint32_t>();
        image.height = reader.Read<std::uint32_t>();

        if (image.width == 0 || image.height == 0 || image.width > kMaxTextureDimension ||
            image.height > kMaxTextureDimension) {
            reader.Fail("has an image with invalid dimensions");
        }

        const std::size_t size = static_cast<std::size_t>(image.width) * image.height * 4;
        const std::span<const std::byte> pixels = reader.ReadBytes(size);
        image.pixels.resize(size);
        CopyBytes(image.pixels.data(), pixels.data(), size);
        return image;
    }

    void WriteVec(BinaryWriter& writer, const float* const values, const std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) { writer.Write(values[i]); }
    }

    void ReadVec(BinaryReader& reader, float* const values, const std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) { values[i] = reader.Read<float>(); }
    }

    void WriteParameters(BinaryWriter& writer, const Frost::MaterialParameters& parameters) {
        WriteVec(writer, &parameters.baseColorFactor.x, 4);
        writer.Write(parameters.metallicFactor);
        writer.Write(parameters.roughnessFactor);
        writer.Write(parameters.normalScale);
        writer.Write(parameters.occlusionStrength);
        WriteVec(writer, &parameters.emissiveFactor.x, 3);
    }

    [[nodiscard]]
    Frost::MaterialParameters ReadParameters(BinaryReader& reader) {
        Frost::MaterialParameters parameters;
        ReadVec(reader, &parameters.baseColorFactor.x, 4);
        parameters.metallicFactor = reader.Read<float>();
        parameters.roughnessFactor = reader.Read<float>();
        parameters.normalScale = reader.Read<float>();
        parameters.occlusionStrength = reader.Read<float>();
        ReadVec(reader, &parameters.emissiveFactor.x, 3);
        return parameters;
    }

    void WriteGeometry(BinaryWriter& writer, const Frost::MeshData& mesh) {
        // Vertices, field by field (not the in-memory struct, whose padding is the compiler's
        // business).
        writer.Write(static_cast<std::uint32_t>(mesh.vertices.size()));
        for (const Frost::Vertex& vertex : mesh.vertices) {
            WriteVec(writer, &vertex.position.x, 3);
            WriteVec(writer, &vertex.normal.x, 3);
            WriteVec(writer, &vertex.uv.x, 2);
            WriteVec(writer, &vertex.color.x, 4);
            WriteVec(writer, &vertex.tangent.x, 4);
        }

        writer.Write(static_cast<std::uint32_t>(mesh.indices.size()));
        writer.WriteBytes(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));

        writer.Write(static_cast<std::uint32_t>(mesh.subMeshes.size()));
        for (const Frost::SubMesh& subMesh : mesh.subMeshes) {
            writer.Write(subMesh.firstIndex);
            writer.Write(subMesh.indexCount);
            writer.Write(subMesh.vertexOffset);
            writer.Write(subMesh.materialIndex);
            writer.WriteString(subMesh.name);
        }
    }

    void ReadGeometry(BinaryReader& reader, Frost::MeshData& mesh) {
        mesh.vertices.resize(reader.ReadCount(kFloatsPerVertex * sizeof(float), "vertices"));
        for (Frost::Vertex& vertex : mesh.vertices) {
            ReadVec(reader, &vertex.position.x, 3);
            ReadVec(reader, &vertex.normal.x, 3);
            ReadVec(reader, &vertex.uv.x, 2);
            ReadVec(reader, &vertex.color.x, 4);
            ReadVec(reader, &vertex.tangent.x, 4);
        }

        mesh.indices.resize(reader.ReadCount(sizeof(std::uint32_t), "indices"));
        const std::span<const std::byte> indexBytes = reader.ReadBytes(mesh.indices.size() * sizeof(std::uint32_t));
        CopyBytes(mesh.indices.data(), indexBytes.data(), indexBytes.size());

        // Each sub-mesh is at least 20 bytes (four values and an empty name's length).
        mesh.subMeshes.resize(reader.ReadCount(20, "sub-meshes"));
        for (Frost::SubMesh& subMesh : mesh.subMeshes) {
            subMesh.firstIndex = reader.Read<std::uint32_t>();
            subMesh.indexCount = reader.Read<std::uint32_t>();
            subMesh.vertexOffset = reader.Read<std::int32_t>();
            subMesh.materialIndex = reader.Read<std::int32_t>();
            subMesh.name = reader.ReadString();
        }
    }

    // Cross-references of the geometry: sub-mesh ranges, index values and material indices. Mesh
    // itself checks that sub-mesh ranges fit the index buffer, but not the index values, which
    // would make the GPU read outside the vertex buffer.
    void ValidateGeometry(const BinaryReader& reader, const Frost::MeshData& mesh, const std::size_t materialCount) {
        if (mesh.vertices.empty() || mesh.indices.empty()) { reader.Fail("has no geometry"); }

        const auto vertexCount = static_cast<std::int64_t>(mesh.vertices.size());
        for (const Frost::SubMesh& subMesh : mesh.subMeshes) {
            const std::uint64_t end = std::uint64_t{subMesh.firstIndex} + subMesh.indexCount;
            if (end > mesh.indices.size()) { reader.Fail("has a sub-mesh outside its index buffer"); }

            for (std::uint64_t i = subMesh.firstIndex; i < end; ++i) {
                const std::int64_t vertex = std::int64_t{mesh.indices[i]} + subMesh.vertexOffset;
                if (vertex < 0 || vertex >= vertexCount) { reader.Fail("has an index outside its vertex buffer"); }
            }

            if (subMesh.materialIndex < -1 || subMesh.materialIndex >= static_cast<std::int32_t>(materialCount)) {
                reader.Fail("has a sub-mesh referencing a material that does not exist");
            }
        }
        if (mesh.subMeshes.empty()) {
            // Drawn as one range with vertex offset 0 (see MeshData::subMeshes).
            for (const std::uint32_t index : mesh.indices) {
                if (index >= mesh.vertices.size()) { reader.Fail("has an index outside its vertex buffer"); }
            }
        }
    }

    // Version 1: materials indexing textures embedded in the file.
    void ReadLegacyMaterialsAndTextures(BinaryReader& reader, Frost::MeshData& mesh) {
        // Each material is at least 68 bytes (an empty name's length, 12 floats and 5 indices).
        mesh.materials.resize(reader.ReadCount(68, "materials"));
        for (Frost::MaterialData& material : mesh.materials) {
            material.name = reader.ReadString();
            material.parameters = ReadParameters(reader);
            for (std::int32_t* const texture : {&material.baseColorTexture, &material.metallicRoughnessTexture,
                                                &material.normalTexture, &material.occlusionTexture,
                                                &material.emissiveTexture}) {
                *texture = reader.Read<std::int32_t>();
            }
        }

        // Each texture is at least 18 bytes (an empty name's length, settings, and dimensions).
        mesh.textures.resize(reader.ReadCount(18, "textures"));
        for (Frost::TextureData& texture : mesh.textures) {
            texture.name = reader.ReadString();
            texture.settings = ReadTextureSettings(reader);
            texture.image = ReadImage(reader);
        }

        for (const Frost::MaterialData& material : mesh.materials) {
            for (const std::int32_t texture : {material.baseColorTexture, material.metallicRoughnessTexture,
                                               material.normalTexture, material.occlusionTexture,
                                               material.emissiveTexture}) {
                if (texture < -1 || texture >= static_cast<std::int32_t>(mesh.textures.size())) {
                    reader.Fail("has a material referencing a texture that does not exist");
                }
            }
        }
    }

    // Materials as meshes and skeletal meshes store them (version 2 and later): parameters, and
    // per texture slot a reference to a texture asset.
    void WriteMaterials(BinaryWriter& writer, const std::vector<Frost::MaterialAssetData>& materials,
                        const std::filesystem::path& projectDirectory) {
        writer.Write(static_cast<std::uint32_t>(materials.size()));
        for (const Frost::MaterialAssetData& material : materials) {
            writer.WriteString(material.name);
            WriteParameters(writer, material.parameters);
            for (const Frost::AssetReference& texture : material.textures) { WriteReference(writer, texture, projectDirectory); }
        }
    }

    [[nodiscard]]
    std::vector<Frost::MaterialAssetData> ReadMaterials(BinaryReader& reader, const std::filesystem::path& projectDirectory) {
        // Each material is at least 152 bytes (an empty name's length, 12 floats, and per slot a
        // 16-byte id and an empty path's length).
        std::vector<Frost::MaterialAssetData> materials(reader.ReadCount(152, "materials"));
        for (Frost::MaterialAssetData& material : materials) {
            material.name = reader.ReadString();
            material.parameters = ReadParameters(reader);
            for (Frost::AssetReference& texture : material.textures) { texture = ReadReference(reader, projectDirectory); }
        }
        return materials;
    }

    void WriteMatrix(BinaryWriter& writer, const glm::mat4& matrix) { WriteVec(writer, &matrix[0][0], 16); }

    [[nodiscard]]
    glm::mat4 ReadMatrix(BinaryReader& reader) {
        glm::mat4 matrix(1.0f);
        ReadVec(reader, &matrix[0][0], 16);
        return matrix;
    }

    void WriteQuaternion(BinaryWriter& writer, const glm::quat& rotation) {
        writer.Write(rotation.x);
        writer.Write(rotation.y);
        writer.Write(rotation.z);
        writer.Write(rotation.w);
    }

    [[nodiscard]]
    glm::quat ReadQuaternion(BinaryReader& reader) {
        const auto x = reader.Read<float>();
        const auto y = reader.Read<float>();
        const auto z = reader.Read<float>();
        const auto w = reader.Read<float>();
        return Frost::MakeQuaternion(x, y, z, w);
    }

    template <typename T>
    void WriteChannel(BinaryWriter& writer, const Frost::AnimationChannel<T>& channel) {
        writer.Write(static_cast<std::uint8_t>(channel.interpolation));
        writer.Write(static_cast<std::uint32_t>(channel.times.size()));
        WriteVec(writer, channel.times.data(), channel.times.size());
        for (const T& value : channel.values) {
            if constexpr (std::is_same_v<T, glm::quat>) { WriteQuaternion(writer, value); }
            else { WriteVec(writer, &value.x, 3); }
        }
    }

    template <typename T>
    void ReadChannel(BinaryReader& reader, Frost::AnimationChannel<T>& channel) {
        constexpr std::size_t kValueBytes = (std::is_same_v<T, glm::quat> ? 4 : 3) * sizeof(float);

        const auto interpolation = reader.Read<std::uint8_t>();
        if (interpolation > static_cast<std::uint8_t>(Frost::AnimationInterpolation::CubicSpline)) {
            reader.Fail("has an animation channel with an unknown interpolation");
        }
        channel.interpolation = static_cast<Frost::AnimationInterpolation>(interpolation);

        // Each key is at least its time and one value.
        channel.times.resize(reader.ReadCount(sizeof(float) + kValueBytes, "keyframes"));
        ReadVec(reader, channel.times.data(), channel.times.size());

        const std::size_t valueCount = channel.times.size() * channel.GetValuesPerKey();
        if (valueCount > reader.Remaining() / kValueBytes) { reader.Fail("declares more keyframes than the file holds"); }
        channel.values.resize(valueCount);
        for (T& value : channel.values) {
            if constexpr (std::is_same_v<T, glm::quat>) { value = ReadQuaternion(reader); }
            else { ReadVec(reader, &value.x, 3); }
        }
    }

    void WriteSoundInfo(BinaryWriter& writer, const Frost::SoundInfo& info) {
        writer.Write(static_cast<std::uint8_t>(info.format));
        writer.Write(info.sampleRate);
        writer.Write(info.channels);
        writer.Write(info.bitsPerSample);
        writer.Write(info.frameCount);
        writer.Write(info.bitRate);
    }

    [[nodiscard]]
    Frost::SoundInfo ReadSoundInfo(BinaryReader& reader) {
        const auto format = reader.Read<std::uint8_t>();
        if (format > static_cast<std::uint8_t>(Frost::SoundFormat::Mp3)) { reader.Fail("has an unknown sound format"); }

        Frost::SoundInfo info;
        info.format = static_cast<Frost::SoundFormat>(format);
        info.sampleRate = reader.Read<std::uint32_t>();
        info.channels = reader.Read<std::uint16_t>();
        info.bitsPerSample = reader.Read<std::uint16_t>();
        info.frameCount = reader.Read<std::uint64_t>();
        info.bitRate = reader.Read<std::uint32_t>();

        const bool validBits = info.format == Frost::SoundFormat::WavPcm
                                       ? info.bitsPerSample == 8 || info.bitsPerSample == 16 || info.bitsPerSample == 24 ||
                                                 info.bitsPerSample == 32
                               : info.format == Frost::SoundFormat::WavFloat ? info.bitsPerSample == 32 || info.bitsPerSample == 64
                                                                              : info.bitsPerSample == 0;
        if (info.channels == 0 || info.channels > Frost::kMaxSoundChannels || info.sampleRate < Frost::kMinSoundSampleRate ||
            info.sampleRate > Frost::kMaxSoundSampleRate || !validBits || info.frameCount == 0) {
            reader.Fail("has invalid sound properties");
        }
        return info;
    }

}  // namespace

namespace Frost {

    const char* AssetFile::GetExtension(const AssetType type) noexcept {
        switch (type) {
            case AssetType::Mesh: return kMeshExtension;
            case AssetType::Texture: return kTextureExtension;
            case AssetType::Skeleton: return kSkeletonExtension;
            case AssetType::SkeletalMesh: return kSkeletalMeshExtension;
            case AssetType::Animation: return kAnimationExtension;
            case AssetType::Sound: return kSoundExtension;
        }
        return "";
    }

    std::uint32_t AssetFile::GetCurrentVersion(const AssetType type) noexcept {
        switch (type) {
            case AssetType::Mesh: return kMeshVersion;
            case AssetType::Texture: return kTextureVersion;
            case AssetType::Skeleton: return kSkeletonVersion;
            case AssetType::SkeletalMesh: return kSkeletalMeshVersion;
            case AssetType::Animation: return kAnimationVersion;
            case AssetType::Sound: return kSoundVersion;
        }
        return 0;
    }

    bool AssetFile::HasId(const AssetType type, const std::uint32_t version) noexcept {
        return (type != AssetType::Mesh && type != AssetType::Texture) || version >= 2;
    }

    void AssetFile::WriteMesh(const std::filesystem::path& path, const MeshFile& file,
                              const std::filesystem::path& projectDirectory) {
        BinaryWriter writer;
        WriteHeader(writer, AssetType::Mesh, path, file.header, projectDirectory);
        WriteGeometry(writer, file.mesh);
        WriteMaterials(writer, file.materials, projectDirectory);
        WriteFileAtomically(path, writer.GetBytes());
    }

    void AssetFile::WriteTexture(const std::filesystem::path& path, const TextureFile& file,
                                 const std::filesystem::path& projectDirectory) {
        BinaryWriter writer;
        WriteHeader(writer, AssetType::Texture, path, file.header, projectDirectory);
        WriteTextureSettings(writer, file.settings);
        WriteImage(writer, file.image);
        WriteFileAtomically(path, writer.GetBytes());
    }

    void AssetFile::WriteSkeleton(const std::filesystem::path& path, const SkeletonFile& file,
                                  const std::filesystem::path& projectDirectory) {
        if (const std::optional<std::string> problem = file.skeleton.Validate()) {
            throw ImportError("Skeleton '" + PathToUtf8(path) + "' cannot be written: " + *problem + ".");
        }

        BinaryWriter writer;
        WriteHeader(writer, AssetType::Skeleton, path, file.header, projectDirectory);
        writer.Write(static_cast<std::uint32_t>(file.skeleton.bones.size()));
        for (const Bone& bone : file.skeleton.bones) {
            writer.WriteString(bone.name);
            writer.Write(bone.parent);
            WriteVec(writer, &bone.restPose.translation.x, 3);
            WriteQuaternion(writer, bone.restPose.rotation);
            WriteVec(writer, &bone.restPose.scale.x, 3);
            writer.Write(static_cast<std::uint8_t>(bone.addedInEditor ? 1 : 0));
        }
        WriteFileAtomically(path, writer.GetBytes());
    }

    void AssetFile::WriteSkeletalMesh(const std::filesystem::path& path, const SkeletalMeshFile& file,
                                      const std::filesystem::path& projectDirectory) {
        if (const std::optional<std::string> problem = ValidateSkin(file.mesh)) {
            throw ImportError("Skeletal mesh '" + PathToUtf8(path) + "' cannot be written: " + *problem + ".");
        }
        if (const std::optional<std::string> problem = ValidateSockets(file.sockets)) {
            throw ImportError("Skeletal mesh '" + PathToUtf8(path) + "' cannot be written: " + *problem + ".");
        }

        BinaryWriter writer;
        WriteHeader(writer, AssetType::SkeletalMesh, path, file.header, projectDirectory);
        WriteReference(writer, file.skeleton, projectDirectory);
        WriteGeometry(writer, file.mesh.mesh);

        writer.Write(static_cast<std::uint32_t>(file.mesh.skin.size()));
        for (const SkinWeights& weights : file.mesh.skin) {
            for (const std::uint16_t bone : weights.bones) { writer.Write(bone); }
            WriteVec(writer, weights.weights.data(), weights.weights.size());
        }

        writer.Write(static_cast<std::uint32_t>(file.mesh.bindBones.size()));
        for (std::size_t i = 0; i < file.mesh.bindBones.size(); ++i) {
            writer.WriteString(file.mesh.bindBones[i]);
            WriteMatrix(writer, file.mesh.inverseBindMatrices[i]);
        }

        writer.Write(static_cast<std::uint32_t>(file.mesh.attachments.size()));
        for (const std::int32_t attachment : file.mesh.attachments) { writer.Write(attachment); }

        writer.Write(static_cast<std::uint32_t>(file.defaultAttachments.size()));
        for (const DefaultAttachment& attachment : file.defaultAttachments) {
            writer.WriteString(attachment.name);
            writer.WriteString(attachment.slot);
            writer.WriteString(attachment.bone);
            WriteReference(writer, attachment.mesh, projectDirectory);
        }

        writer.Write(static_cast<std::uint32_t>(file.sockets.size()));
        for (const SocketData& socket : file.sockets) {
            writer.WriteString(socket.name);
            writer.WriteString(socket.bone);
            WriteVec(writer, &socket.position.x, 3);
            WriteVec(writer, &socket.rotation.x, 3);
            WriteVec(writer, &socket.scale.x, 3);
        }

        WriteMaterials(writer, file.materials, projectDirectory);
        WriteFileAtomically(path, writer.GetBytes());
    }

    void AssetFile::WriteAnimation(const std::filesystem::path& path, const AnimationFile& file,
                                   const std::filesystem::path& projectDirectory) {
        if (const std::optional<std::string> problem = file.clip.Validate()) {
            throw ImportError("Animation '" + PathToUtf8(path) + "' cannot be written: " + *problem + ".");
        }

        BinaryWriter writer;
        WriteHeader(writer, AssetType::Animation, path, file.header, projectDirectory);
        WriteReference(writer, file.skeleton, projectDirectory);
        writer.Write(file.clip.duration);
        writer.Write(static_cast<std::uint32_t>(file.clip.tracks.size()));
        for (const BoneTrack& track : file.clip.tracks) {
            writer.WriteString(track.bone);
            WriteChannel(writer, track.translation);
            WriteChannel(writer, track.rotation);
            WriteChannel(writer, track.scale);
        }

        writer.Write(static_cast<std::uint32_t>(file.clip.visibilityKeys.size()));
        for (const VisibilityKey& key : file.clip.visibilityKeys) {
            writer.Write(key.time);
            writer.WriteString(key.slot);
            writer.Write(static_cast<std::uint8_t>(key.visible ? 1 : 0));
        }
        WriteFileAtomically(path, writer.GetBytes());
    }

    void AssetFile::WriteSound(const std::filesystem::path& path, const SoundFile& file,
                               const std::filesystem::path& projectDirectory) {
        // The embedded file must be the audio the properties describe (Probe throws ImportError if
        // it is no sound at all).
        if (SoundImporter::Probe(file.sound.encoded, PathToUtf8(path)) != file.sound.info) {
            throw ImportError("Sound '" + PathToUtf8(path) + "' cannot be written: its properties do not match its audio.");
        }

        BinaryWriter writer;
        WriteHeader(writer, AssetType::Sound, path, file.header, projectDirectory);
        WriteSoundInfo(writer, file.sound.info);
        writer.Write(static_cast<std::uint64_t>(file.sound.encoded.size()));
        writer.WriteBytes(file.sound.encoded.data(), file.sound.encoded.size());
        WriteFileAtomically(path, writer.GetBytes());
    }

    MeshFile AssetFile::ReadMesh(const std::filesystem::path& path, const std::filesystem::path& projectDirectory) {
        const std::vector<std::byte> bytes = ReadWholeFile(path);
        BinaryReader reader(bytes, "Mesh asset '" + PathToUtf8(path) + "'");

        MeshFile file;
        file.header = ::ReadHeader(reader, AssetType::Mesh, path, projectDirectory);
        ReadGeometry(reader, file.mesh);

        if (file.header.version == 1) {
            ReadLegacyMaterialsAndTextures(reader, file.mesh);
            ValidateGeometry(reader, file.mesh, file.mesh.materials.size());
        }
        else {
            file.materials = ReadMaterials(reader, projectDirectory);
            ValidateGeometry(reader, file.mesh, file.materials.size());
        }

        if (reader.Remaining() != 0) { reader.Fail("has unexpected trailing data"); }

        // Before version 3, positions were in meters; the world is in centimeters. (A uniform scale:
        // normals and tangents are unaffected.)
        if (file.header.version < 3) {
            for (Vertex& vertex : file.mesh.vertices) { vertex.position *= kMeter; }
        }
        return file;
    }

    TextureFile AssetFile::ReadTexture(const std::filesystem::path& path, const std::filesystem::path& projectDirectory) {
        const std::vector<std::byte> bytes = ReadWholeFile(path);
        BinaryReader reader(bytes, "Texture asset '" + PathToUtf8(path) + "'");

        TextureFile file;
        file.header = ::ReadHeader(reader, AssetType::Texture, path, projectDirectory);
        file.settings = ReadTextureSettings(reader);
        file.image = ReadImage(reader);

        if (reader.Remaining() != 0) { reader.Fail("has unexpected trailing data"); }
        return file;
    }

    SkeletonFile AssetFile::ReadSkeleton(const std::filesystem::path& path, const std::filesystem::path& projectDirectory) {
        const std::vector<std::byte> bytes = ReadWholeFile(path);
        BinaryReader reader(bytes, "Skeleton asset '" + PathToUtf8(path) + "'");

        SkeletonFile file;
        file.header = ::ReadHeader(reader, AssetType::Skeleton, path, projectDirectory);

        file.skeleton.bones.resize(reader.ReadCount(kMinBoneBytes, "bones"));
        for (Bone& bone : file.skeleton.bones) {
            bone.name = reader.ReadString();
            bone.parent = reader.Read<std::int32_t>();
            ReadVec(reader, &bone.restPose.translation.x, 3);
            bone.restPose.rotation = ReadQuaternion(reader);
            ReadVec(reader, &bone.restPose.scale.x, 3);
            // Version 2: flags.
            if (file.header.version >= 2) {
                const auto flags = reader.Read<std::uint8_t>();
                if (flags > 1) { reader.Fail("has invalid bone flags"); }
                bone.addedInEditor = (flags & 1) != 0;
            }
        }

        if (reader.Remaining() != 0) { reader.Fail("has unexpected trailing data"); }
        if (const std::optional<std::string> problem = file.skeleton.Validate()) { reader.Fail("is invalid: " + *problem); }
        return file;
    }

    SkeletalMeshFile AssetFile::ReadSkeletalMesh(const std::filesystem::path& path,
                                                 const std::filesystem::path& projectDirectory) {
        const std::vector<std::byte> bytes = ReadWholeFile(path);
        BinaryReader reader(bytes, "Skeletal mesh asset '" + PathToUtf8(path) + "'");

        SkeletalMeshFile file;
        file.header = ::ReadHeader(reader, AssetType::SkeletalMesh, path, projectDirectory);
        file.skeleton = ReadReference(reader, projectDirectory);
        ReadGeometry(reader, file.mesh.mesh);

        file.mesh.skin.resize(reader.ReadCount(kSkinWeightBytes, "skin weights"));
        for (SkinWeights& weights : file.mesh.skin) {
            for (std::uint16_t& bone : weights.bones) { bone = reader.Read<std::uint16_t>(); }
            ReadVec(reader, weights.weights.data(), weights.weights.size());
        }

        const std::uint32_t bindCount = reader.ReadCount(kMinBindEntryBytes, "bind table entries");
        file.mesh.bindBones.resize(bindCount);
        file.mesh.inverseBindMatrices.resize(bindCount);
        for (std::uint32_t i = 0; i < bindCount; ++i) {
            file.mesh.bindBones[i] = reader.ReadString();
            file.mesh.inverseBindMatrices[i] = ReadMatrix(reader);
        }

        // Version 2: sub-meshes attached to bones (version 1 had none).
        if (file.header.version >= 2) {
            file.mesh.attachments.resize(reader.ReadCount(sizeof(std::int32_t), "attachments"));
            for (std::int32_t& attachment : file.mesh.attachments) { attachment = reader.Read<std::int32_t>(); }
        }

        // Version 3: default attachments (each at least three empty strings and a reference).
        if (file.header.version >= 3) {
            file.defaultAttachments.resize(reader.ReadCount(4 * 3 + kReferenceBytes, "default attachments"));
            for (DefaultAttachment& attachment : file.defaultAttachments) {
                attachment.name = reader.ReadString();
                attachment.slot = reader.ReadString();
                attachment.bone = reader.ReadString();
                attachment.mesh = ReadReference(reader, projectDirectory);
            }
        }

        // Version 4: sockets (each at least two empty strings and nine floats).
        if (file.header.version >= 4) {
            file.sockets.resize(reader.ReadCount(4 * 2 + 9 * 4, "sockets"));
            for (SocketData& socket : file.sockets) {
                socket.name = reader.ReadString();
                socket.bone = reader.ReadString();
                ReadVec(reader, &socket.position.x, 3);
                ReadVec(reader, &socket.rotation.x, 3);
                ReadVec(reader, &socket.scale.x, 3);
            }
            if (const std::optional<std::string> problem = ValidateSockets(file.sockets)) { reader.Fail("is invalid: " + *problem); }
        }

        file.materials = ReadMaterials(reader, projectDirectory);
        ValidateGeometry(reader, file.mesh.mesh, file.materials.size());

        if (reader.Remaining() != 0) { reader.Fail("has unexpected trailing data"); }
        if (const std::optional<std::string> problem = ValidateSkin(file.mesh)) { reader.Fail("is invalid: " + *problem); }
        return file;
    }

    AnimationFile AssetFile::ReadAnimation(const std::filesystem::path& path, const std::filesystem::path& projectDirectory) {
        const std::vector<std::byte> bytes = ReadWholeFile(path);
        BinaryReader reader(bytes, "Animation asset '" + PathToUtf8(path) + "'");

        AnimationFile file;
        file.header = ::ReadHeader(reader, AssetType::Animation, path, projectDirectory);
        file.skeleton = ReadReference(reader, projectDirectory);
        file.clip.duration = reader.Read<float>();

        file.clip.tracks.resize(reader.ReadCount(kMinTrackBytes, "tracks"));
        for (BoneTrack& track : file.clip.tracks) {
            track.bone = reader.ReadString();
            ReadChannel(reader, track.translation);
            ReadChannel(reader, track.rotation);
            ReadChannel(reader, track.scale);
        }

        // Version 2: visibility keys (each at least a time, an empty slot's length and a flag).
        if (file.header.version >= 2) {
            file.clip.visibilityKeys.resize(reader.ReadCount(4 + 4 + 1, "visibility keys"));
            for (VisibilityKey& key : file.clip.visibilityKeys) {
                key.time = reader.Read<float>();
                key.slot = reader.ReadString();
                const auto visible = reader.Read<std::uint8_t>();
                if (visible > 1) { reader.Fail("has an invalid visibility key"); }
                key.visible = visible != 0;
            }
        }

        if (reader.Remaining() != 0) { reader.Fail("has unexpected trailing data"); }
        if (const std::optional<std::string> problem = file.clip.Validate()) { reader.Fail("is invalid: " + *problem); }
        return file;
    }

    SoundFile AssetFile::ReadSound(const std::filesystem::path& path, const std::filesystem::path& projectDirectory) {
        const std::vector<std::byte> bytes = ReadWholeFile(path);
        BinaryReader reader(bytes, "Sound asset '" + PathToUtf8(path) + "'");

        SoundFile file;
        file.header = ::ReadHeader(reader, AssetType::Sound, path, projectDirectory);
        file.sound.info = ::ReadSoundInfo(reader);

        const auto size = reader.Read<std::uint64_t>();
        if (size == 0 || size > kMaxSoundFileBytes) { reader.Fail("has an invalid audio size"); }
        if (size > reader.Remaining()) { reader.Fail("ends unexpectedly"); }
        const std::span<const std::byte> encoded = reader.ReadBytes(static_cast<std::size_t>(size));
        file.sound.encoded.assign(encoded.begin(), encoded.end());

        if (reader.Remaining() != 0) { reader.Fail("has unexpected trailing data"); }
        return file;
    }

    std::optional<SoundInfo> AssetFile::ReadSoundInfo(const std::filesystem::path& path) {
        // Only the header and the properties are read, however long the audio.
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) { return std::nullopt; }

        std::vector<char> chars(kMaxHeaderBytes + kSoundInfoBytes);
        stream.read(chars.data(), static_cast<std::streamsize>(chars.size()));
        chars.resize(static_cast<std::size_t>(stream.gcount()));

        std::vector<std::byte> bytes(chars.size());
        CopyBytes(bytes.data(), chars.data(), chars.size());

        try {
            BinaryReader reader(bytes, PathToUtf8(path));
            (void)::ReadHeader(reader, AssetType::Sound, path, {});
            return ::ReadSoundInfo(reader);
        }
        catch (const ImportError&) {
            return std::nullopt;
        }
    }

    std::optional<AssetHeader> AssetFile::ReadHeader(const std::filesystem::path& path,
                                                     const std::filesystem::path& projectDirectory) {
        // Only the header is needed: magic, version, id, and two bounded strings.
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) { return std::nullopt; }

        std::vector<char> chars(32 + 2 * (4 + kMaxStringBytes));
        stream.read(chars.data(), static_cast<std::streamsize>(chars.size()));
        chars.resize(static_cast<std::size_t>(stream.gcount()));

        std::vector<std::byte> bytes(chars.size());
        CopyBytes(bytes.data(), chars.data(), chars.size());

        try {
            BinaryReader probe(bytes, PathToUtf8(path));
            const std::array<char, 4> magic = probe.ReadMagic();

            for (const AssetType type : kAssetTypes) {
                if (magic == MagicOf(type)) {
                    BinaryReader reader(bytes, PathToUtf8(path));
                    return ::ReadHeader(reader, type, path, projectDirectory);
                }
            }
            return std::nullopt;
        }
        catch (const ImportError&) {
            return std::nullopt;
        }
    }

    void AssetFile::RewriteId(const std::filesystem::path& path, const AssetId& id) {
        const std::optional<AssetHeader> header = ReadHeader(path, {});
        if (!header.has_value()) { throw ImportError("'" + PathToUtf8(path) + "' is not a Frost asset."); }
        if (!HasId(header->type, header->version)) {
            throw ImportError("'" + PathToUtf8(path) + "' predates asset ids; upgrade it first.");
        }
        if (!id.IsValid()) { throw ImportError("Cannot give '" + PathToUtf8(path) + "' an empty id."); }

        // The id is at a fixed offset (after the magic and the version): patch it in place, on a
        // copy written atomically like every other asset write.
        std::vector<std::byte> bytes = ReadWholeFile(path);
        BinaryWriter idBytes;
        WriteId(idBytes, id);
        std::memcpy(bytes.data() + 8, idBytes.GetBytes().data(), idBytes.GetBytes().size());

        WriteFileAtomically(path, bytes);
    }

}  // namespace Frost
