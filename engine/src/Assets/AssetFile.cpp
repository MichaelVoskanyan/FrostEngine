#include <Frost/Assets/AssetFile.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Units.hpp>

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

    // Sanity limits for counts read from a file, so a corrupt count fails cleanly instead of
    // attempting a huge allocation. Generous for real content.
    constexpr std::uint32_t kMaxStringBytes = 64 * 1024;
    constexpr std::uint32_t kMaxTextureDimension = 16384;

    // Floats per vertex on disk: position 3, normal 3, uv 2, color 4, tangent 4.
    constexpr std::size_t kFloatsPerVertex = 16;

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

    void WriteHeader(BinaryWriter& writer, const std::array<char, 4>& magic, const std::uint32_t version,
                     const std::filesystem::path& assetPath, const Frost::AssetHeader& header,
                     const std::filesystem::path& projectDirectory) {
        if (!header.id.IsValid()) {
            throw Frost::ImportError("Asset '" + Frost::PathToUtf8(assetPath) + "' cannot be written without an id.");
        }

        writer.WriteMagic(magic);
        writer.Write(version);
        WriteId(writer, header.id);
        WritePath(writer, header.sourcePath, projectDirectory);
        writer.WriteString(header.sourceItem);
    }

    // Reads the header of a file expected to be of one type (either version).
    [[nodiscard]]
    Frost::AssetHeader ReadHeader(BinaryReader& reader, const std::array<char, 4>& magic, const Frost::AssetType type,
                                  const std::uint32_t supportedVersion, const std::filesystem::path& assetPath,
                                  const std::filesystem::path& projectDirectory) {
        const char* const kind = type == Frost::AssetType::Mesh ? "mesh" : "texture";
        if (reader.ReadMagic() != magic) { reader.Reject(std::string("is not a Frost ") + kind + " asset"); }

        Frost::AssetHeader header;
        header.type = type;
        header.version = reader.Read<std::uint32_t>();
        if (header.version == 0) { reader.Fail("has an invalid format version"); }
        if (header.version > supportedVersion) {
            reader.Reject("has format version " + std::to_string(header.version) +
                          ", which this build does not support (" + std::to_string(supportedVersion) +
                          " or older); it was saved by a newer version of Frost");
        }

        // Version 1 had no id and no source item.
        if (header.version >= 2) { header.id = ReadId(reader); }
        const bool legacy = header.version < 2;
        header.sourcePath = ReadPath(reader, legacy ? AssetDirectory(assetPath) : projectDirectory, legacy);
        if (header.version >= 2) { header.sourceItem = reader.ReadString(); }
        return header;
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

}  // namespace

namespace Frost {

    void AssetFile::WriteMesh(const std::filesystem::path& path, const MeshFile& file,
                              const std::filesystem::path& projectDirectory) {
        BinaryWriter writer;
        WriteHeader(writer, kMeshMagic, kMeshVersion, path, file.header, projectDirectory);
        WriteGeometry(writer, file.mesh);

        writer.Write(static_cast<std::uint32_t>(file.materials.size()));
        for (const MaterialAssetData& material : file.materials) {
            writer.WriteString(material.name);
            WriteParameters(writer, material.parameters);
            for (const AssetReference& texture : material.textures) {
                WriteId(writer, texture.id);
                WritePath(writer, texture.path, projectDirectory);
            }
        }

        WriteFileAtomically(path, writer.GetBytes());
    }

    void AssetFile::WriteTexture(const std::filesystem::path& path, const TextureFile& file,
                                 const std::filesystem::path& projectDirectory) {
        BinaryWriter writer;
        WriteHeader(writer, kTextureMagic, kTextureVersion, path, file.header, projectDirectory);
        WriteTextureSettings(writer, file.settings);
        WriteImage(writer, file.image);
        WriteFileAtomically(path, writer.GetBytes());
    }

    MeshFile AssetFile::ReadMesh(const std::filesystem::path& path, const std::filesystem::path& projectDirectory) {
        const std::vector<std::byte> bytes = ReadWholeFile(path);
        BinaryReader reader(bytes, "Mesh asset '" + PathToUtf8(path) + "'");

        MeshFile file;
        file.header = ::ReadHeader(reader, kMeshMagic, AssetType::Mesh, kMeshVersion, path, projectDirectory);
        ReadGeometry(reader, file.mesh);

        if (file.header.version == 1) {
            ReadLegacyMaterialsAndTextures(reader, file.mesh);
            ValidateGeometry(reader, file.mesh, file.mesh.materials.size());
        }
        else {
            // Each material is at least 152 bytes (an empty name's length, 12 floats, and per slot
            // a 16-byte id and an empty path's length).
            file.materials.resize(reader.ReadCount(152, "materials"));
            for (MaterialAssetData& material : file.materials) {
                material.name = reader.ReadString();
                material.parameters = ReadParameters(reader);
                for (AssetReference& texture : material.textures) {
                    texture.id = ReadId(reader);
                    texture.path = ReadPath(reader, projectDirectory, false);
                }
            }

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
        file.header = ::ReadHeader(reader, kTextureMagic, AssetType::Texture, kTextureVersion, path, projectDirectory);
        file.settings = ReadTextureSettings(reader);
        file.image = ReadImage(reader);

        if (reader.Remaining() != 0) { reader.Fail("has unexpected trailing data"); }
        return file;
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

            BinaryReader reader(bytes, PathToUtf8(path));
            if (magic == kMeshMagic) {
                return ::ReadHeader(reader, kMeshMagic, AssetType::Mesh, kMeshVersion, path, projectDirectory);
            }
            if (magic == kTextureMagic) {
                return ::ReadHeader(reader, kTextureMagic, AssetType::Texture, kTextureVersion, path, projectDirectory);
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
        if (header->version < 2) { throw ImportError("'" + PathToUtf8(path) + "' predates asset ids; upgrade it first."); }
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
