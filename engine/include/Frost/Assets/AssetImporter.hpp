#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Assets/AssetType.hpp>
#include <Frost/Assets/ImportError.hpp>

#include <filesystem>
#include <optional>
#include <vector>

namespace Frost {

    class AssetRegistry;

    // Turns source files into Frost assets (see AssetFile): the one step where third-party formats
    // are read. A model (.gltf/.glb, through GltfImporter) becomes a .fmesh, and each texture it
    // contains becomes a .ftex of its own, which the mesh's materials refer to (so the textures can
    // also be used on their own, e.g. applied to other objects). An image (.png/.jpg, through
    // ImageImporter) becomes a .ftex. Assets record what they were imported from, for reimporting.
    //
    // Reimporting into an existing asset keeps its id, and a model's textures keep theirs (each is
    // matched to the texture of the source it came from), so nothing that refers to them breaks.
    //
    // Used by tools (the editor); the game only ever loads the resulting assets.
    class AssetImporter final {
    public:
        AssetImporter() = delete;

        struct Options {
            // The asset to write (replaced, keeping its id, if it exists).
            std::filesystem::path destination{};
            // Where a model's textures go; empty = GetDefaultTextureFolder(destination).
            std::filesystem::path textureFolder{};
            // For finding the textures an earlier import of the same model produced (so they are
            // updated rather than duplicated). Optional; without it textures are always new.
            const AssetRegistry* registry = nullptr;
            // The project directory paths are stored relative to (see AssetFile); empty = the
            // registry's.
            std::filesystem::path projectDirectory{};
        };

        struct Result {
            AssetReference asset;
            std::vector<AssetReference> textures;  // A model's textures, as written.
        };

        // What a source file imports as, judged by its extension (case-insensitive); nullopt if no
        // importer handles it.
        [[nodiscard]]
        static std::optional<AssetType> GetSourceType(const std::filesystem::path& source);

        // The asset file extension for a type (".fmesh", ".ftex").
        [[nodiscard]]
        static const char* GetAssetExtension(AssetType type) noexcept;

        // Where a model's textures go by default: the "Textures" folder next to the mesh's folder
        // if the mesh is in a folder called "Meshes" (Weapons/Glock19/Meshes ->
        // Weapons/Glock19/Textures), else a "Textures" folder inside the mesh's folder.
        [[nodiscard]]
        static std::filesystem::path GetDefaultTextureFolder(const std::filesystem::path& meshPath);

        // Reads `source` and writes the asset(s). Throws ImportError if the source is unsupported,
        // unreadable or malformed, or an asset cannot be written.
        static Result Import(const std::filesystem::path& source, const Options& options);

        // Brings a version-1 asset (from before ids) up to date in place: it gets an id, and a
        // mesh's embedded textures become .ftex assets in its default texture folder. The project
        // directory is the registry's. Throws ImportError.
        static Result UpgradeLegacyAsset(const std::filesystem::path& asset, const AssetRegistry* registry);
    };

}  // namespace Frost
