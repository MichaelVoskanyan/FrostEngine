#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Assets/AssetType.hpp>
#include <Frost/Assets/Image.hpp>
#include <Frost/Assets/ImportError.hpp>
#include <Frost/Renderer/Material.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Frost {

    // What every asset file starts with (see AssetFile::ReadHeader).
    struct AssetHeader {
        AssetType type{};
        std::uint32_t version = 0;
        // The asset's permanent identity. Invalid only in version-1 files, which predate ids (see
        // AssetImporter::UpgradeLegacyAsset).
        AssetId id;
        // The file it was imported from (resolved to an absolute path), for reimporting, and which
        // item inside that file (e.g. one of a model's textures); both empty if unknown / the whole
        // file.
        std::filesystem::path sourcePath;
        std::string sourceItem;
    };

    // A material as a mesh asset stores it: parameters, and per texture slot a reference to a
    // texture asset (unset = the slot's neutral default).
    struct MaterialAssetData {
        std::string name;
        MaterialParameters parameters;
        std::array<AssetReference, MaterialTextures::kSlotCount> textures{};
    };

    // The contents of a .fmesh.
    struct MeshFile {
        AssetHeader header;
        // Vertices, indices and sub-meshes; each sub-mesh's materialIndex indexes `materials`.
        // (For version-1 files only: the materials and textures the file embedded are in
        // mesh.materials/mesh.textures instead, and `materials` is empty.)
        MeshData mesh;
        std::vector<MaterialAssetData> materials;
    };

    // The contents of a .ftex.
    struct TextureFile {
        AssetHeader header;
        ImageData image;
        TextureSettings settings;
    };

    // Frost's own asset files: what the editor's importers produce from source files (glTF, PNG,
    // ...) and what levels and the runtime load. They hold the engine's data structures directly,
    // so loading one is a straight read with no parsing of third-party formats, and every source
    // format the importers learn later (FBX, skinned meshes, ...) ends up in the same files.
    //
    //   .fmesh - geometry, sub-meshes and materials. Materials refer to their textures, which are
    //            separate .ftex assets, by reference (id + last known path).
    //   .ftex  - an RGBA8 image plus the sampler settings it was imported with.
    //
    // Layout (version 2): little-endian throughout. A common header, then the type's payload:
    //   char[4] magic ("FMSH" / "FTEX"), u32 version, u64 idHigh, u64 idLow,
    //   string sourcePath, string sourceItem
    // where a string is a u32 byte length followed by UTF-8 bytes. The paths a file carries (its
    // source, a material's texture paths) are stored relative to the project directory when inside
    // it (see MakeProjectPath), so they stay valid when assets are moved around the project or the
    // project is moved; every function here takes that directory (conventionally the content
    // directory's parent, see AssetRegistry::GetProjectDirectory). The id sits at a fixed offset
    // (8) so it can be rewritten in place.
    //
    // Mesh positions are in world units (centimeters, see Frost/Units.hpp) from mesh version 3;
    // versions 1 and 2 stored them in meters (glTF's unit), and are converted as they are read, so
    // every mesh reads back in centimeters whatever its version.
    //
    // Version 1 (no id, no source item; meshes embedded their textures) is still read. Readers
    // reject newer versions, and validate every count and index against the file, so a truncated
    // or corrupt asset is reported (ImportError) rather than read out of bounds.
    class AssetFile final {
    public:
        AssetFile() = delete;

        static constexpr const char* kMeshExtension = ".fmesh";
        static constexpr const char* kTextureExtension = ".ftex";

        // 3: positions in centimeters (before: meters).
        static constexpr std::uint32_t kMeshVersion = 3;
        static constexpr std::uint32_t kTextureVersion = 2;

        // Write the current version (the header's type/version are ignored; its id must be valid).
        // Atomic (a temporary file renamed over the target), creating the directory if needed.
        // Throw ImportError if the file cannot be written.
        static void WriteMesh(const std::filesystem::path& path, const MeshFile& mesh,
                              const std::filesystem::path& projectDirectory);
        static void WriteTexture(const std::filesystem::path& path, const TextureFile& texture,
                                 const std::filesystem::path& projectDirectory);

        // Throw ImportError if the file is missing, not of this type, from a newer version, or
        // malformed.
        [[nodiscard]]
        static MeshFile ReadMesh(const std::filesystem::path& path, const std::filesystem::path& projectDirectory);

        [[nodiscard]]
        static TextureFile ReadTexture(const std::filesystem::path& path, const std::filesystem::path& projectDirectory);

        // Reads only the header. Nullopt if the file cannot be read or is not a Frost asset.
        [[nodiscard]]
        static std::optional<AssetHeader> ReadHeader(const std::filesystem::path& path,
                                                     const std::filesystem::path& projectDirectory);

        // Gives an existing (version 2 or later) asset file a different id, in place; e.g. for a
        // copy of an asset, which must not share the original's identity. Throws ImportError.
        static void RewriteId(const std::filesystem::path& path, const AssetId& id);
    };

}  // namespace Frost
