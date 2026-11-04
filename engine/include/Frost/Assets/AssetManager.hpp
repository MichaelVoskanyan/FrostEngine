#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Assets/AssetRegistry.hpp>
#include <Frost/Assets/AssetType.hpp>
#include <Frost/Assets/ImportError.hpp>
#include <Frost/Scene/Bounds.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Frost {

    class Renderer;
    class Mesh;
    class Texture;
    class MaterialSystem;

    // A loaded mesh asset (.fmesh): the GPU mesh (with the materials the model came with attached
    // to its sub-meshes) plus a CPU copy of its positions and indices in the mesh's object space,
    // for picking and bounds. Immutable once loaded; shared by every object that places it.
    struct MeshAsset {
        AssetId id;                  // Invalid only for version-1 files.
        std::filesystem::path path;  // Where it was loaded from (the file may have moved since; see AssetRegistry).
        std::string name;            // File name without extension, at load time.
        std::shared_ptr<Mesh> mesh;
        std::vector<glm::vec3> positions;
        std::vector<std::uint32_t> indices;  // Into `positions`, per sub-mesh offset by its vertexOffset.
        Bounds bounds;
        // Texture references of its materials that could not be loaded (those slots use their
        // defaults). Non-zero means reloading may improve it once the textures are found.
        int missingTextures = 0;
    };

    // Loads Frost assets (.fmesh, .ftex; see AssetFile) and keeps them loaded, one instance per
    // asset: loading the same asset again returns the cached one, even if its file has moved.
    // Owned by Engine; shared by the game and the editor. Source files (glTF, PNG, ...) are not
    // loaded here: they are turned into assets first, by AssetImporter.
    //
    // Assets are found through an AssetRegistry of the project's content directory: references are
    // resolved by asset id first, so they survive files being renamed or moved, and only ever to
    // files inside the content directory (a project uses its own assets, never another project's).
    //
    // Textures are cached per color space: the same image is uploaded once as sRGB data (for base
    // color / emissive slots) and once as linear data (everything else) if it is used both ways.
    //
    // Loading is synchronous: the file is read and the upload completes before the call returns.
    class AssetManager final {
    public:
        // The renderer must outlive the manager (Engine guarantees it). `contentDirectory` (the
        // project's; see Project) is scanned into the registry (empty = none, for tools working
        // outside a project; references then resolve by path only).
        AssetManager(Renderer& renderer, const std::filesystem::path& contentDirectory);
        ~AssetManager();

        AssetManager(const AssetManager&) = delete;
        AssetManager& operator=(const AssetManager&) = delete;

        AssetManager(AssetManager&&) = delete;
        AssetManager& operator=(AssetManager&&) = delete;

        [[nodiscard]]
        AssetRegistry& GetRegistry() noexcept;

        [[nodiscard]]
        const AssetRegistry& GetRegistry() const noexcept;

        // Loads (or returns the cached) mesh asset the reference points at. Throws ImportError if
        // it cannot be found, is not a mesh asset (a source file such as a .gltf must be imported
        // first), or is malformed. A texture its materials refer to that cannot be loaded is
        // reported and its slot left at the default, rather than failing the whole mesh.
        [[nodiscard]]
        std::shared_ptr<const MeshAsset> LoadMesh(const AssetReference& reference);

        // By file path (the file's header supplies the id).
        [[nodiscard]]
        std::shared_ptr<const MeshAsset> LoadMesh(const std::filesystem::path& path);

        // Loads (or returns the cached) texture asset, sampled as sRGB color (`srgb`) or as linear
        // data. Throws ImportError like LoadMesh.
        [[nodiscard]]
        std::shared_ptr<Texture> LoadTexture(const AssetReference& reference, bool srgb);

        [[nodiscard]]
        std::shared_ptr<Texture> LoadTexture(const std::filesystem::path& path, bool srgb);

        // Drops the cached asset(s), so the next load reads the file again (e.g. after it was
        // reimported). Objects still holding the old asset keep it alive until they let go.
        void Unload(const AssetReference& reference);

        // Drops every cached texture (e.g. after textures were reimported; meshes keep the
        // textures they were built with until they are reloaded).
        void UnloadTextures();

        // Drops every cached asset.
        void UnloadAll();

        [[nodiscard]]
        const MaterialSystem& GetMaterialSystem() const noexcept;

        // The canonical form used for paths: absolute, with "." and ".." resolved (and symlinks,
        // where the path exists).
        [[nodiscard]]
        static std::filesystem::path NormalizePath(const std::filesystem::path& path);

        // The type of a Frost asset file, judged by its extension (.fmesh, .ftex; case-insensitive);
        // nullopt for anything else (including source files: see AssetImporter::GetSourceType).
        [[nodiscard]]
        static std::optional<AssetType> GetAssetType(const std::filesystem::path& path);

    private:
        // Cache key: the asset id, or the path for version-1 files (which have none).
        [[nodiscard]]
        static std::string CacheKey(const AssetId& id, const std::filesystem::path& path);

        // The file a reference points at now; throws ImportError if it cannot be found.
        [[nodiscard]]
        std::filesystem::path ResolveOrThrow(const AssetReference& reference, AssetType type) const;

        Renderer& m_Renderer;
        AssetRegistry m_Registry;

        std::map<std::string, std::shared_ptr<const MeshAsset>> m_Meshes;
        std::map<std::pair<std::string, bool>, std::shared_ptr<Texture>> m_Textures;  // (key, srgb).
    };

}  // namespace Frost
