#pragma once

#include <Frost/Animation/AnimationClip.hpp>
#include <Frost/Animation/Skeleton.hpp>
#include <Frost/Animation/SkinnedMesh.hpp>
#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetId.hpp>
#include <Frost/Assets/AssetRegistry.hpp>
#include <Frost/Assets/AssetType.hpp>
#include <Frost/Assets/ImportError.hpp>
#include <Frost/Scene/Bounds.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Frost {

    class Renderer;
    struct MaterialAssetData;
    struct SkinnedVertex;
    class Material;
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

    // A loaded skeleton asset (.fskel). Immutable once loaded.
    struct SkeletonAsset {
        AssetId id;
        std::filesystem::path path;  // Where it was loaded from.
        std::string name;            // File name without extension, at load time.
        SkeletonData skeleton;
        // Every bone's model-space transform in the rest pose (by bone index).
        std::vector<glm::mat4> restTransforms;
    };

    // A loaded animation asset (.fanim). Immutable once loaded. Played on a skeleton by bone name
    // (see AnimationClipData::BindTracks), so the skeleton it was imported for is only a record.
    struct AnimationAsset {
        AssetId id;
        std::filesystem::path path;
        std::string name;
        AssetReference skeleton;  // The skeleton it was imported for.
        AnimationClipData clip;
    };

    // A loaded skeletal mesh asset (.fskmesh): its skin and bind-pose geometry, the skeleton it is
    // bound to, its materials, and its drawable: the mesh on the GPU with its skin stream, deformed
    // there by a joint palette (see ComputePalette and Renderer::SubmitSkinned). Immutable once
    // loaded; shared by every object that places it.
    //
    // The palette is the bind table's skinning matrices (bone model-space transform x inverse bind
    // matrix), then one matrix per attached sub-mesh (see SkeletalMeshData::attachments): its
    // bone's skinning matrix when attachments follow the pose, else the one of the rest pose (the
    // part stays where it was modeled). The drawable's skin stream indexes it.
    struct SkeletalMeshAsset {
        AssetId id;
        std::filesystem::path path;
        std::string name;
        AssetReference skeletonReference;
        // Null if the skeleton could not be loaded; the mesh is then shown in its bind pose (as
        // modeled) and cannot be posed.
        std::shared_ptr<const SkeletonAsset> skeleton;
        // Geometry (without materials or textures), skin and bind table, on the CPU for posing.
        SkeletalMeshData data;
        // Per bind table entry, its bone in `skeleton` (-1: the skeleton has no bone of that name;
        // what it moves stays in the bind pose).
        std::vector<std::int32_t> bindings;
        // Per sub-mesh, its palette slot if it is attached to a bone, else -1.
        std::vector<std::int32_t> attachmentSlots;
        std::size_t paletteSize = 0;
        // Per vertex, its influences as palette indices (the skin, with attached sub-meshes fully
        // weighted to their slots): the CPU copy of the drawable's skin stream, for posing
        // positions on the CPU (picking, bounds).
        std::vector<SkinWeights> paletteSkin;
        // The palette of the skeleton's rest pose (identity matrices without a skeleton).
        std::vector<glm::mat4> restPalette;
        // Per sub-mesh, the material the model came with (null: none, drawn with the default).
        std::vector<std::shared_ptr<Material>> materials;
        // What is drawn: the bind-pose mesh with its skin stream, on the GPU. Its CPU positions and
        // bounds are those of the rest pose.
        std::shared_ptr<const MeshAsset> drawable;
        int missingTextures = 0;  // As MeshAsset::missingTextures.
        int missingBones = 0;     // Bind table entries whose bone the skeleton does not have.
        // The parts it comes with as separate objects (see DefaultAttachment); not loaded here.
        std::vector<DefaultAttachment> defaultAttachments;
        // Its sockets (see SocketData).
        std::vector<SocketData> sockets;

        // The socket of this name, or null.
        [[nodiscard]]
        const SocketData* FindSocket(std::string_view socketName) const noexcept;

        // The palette for a pose: `boneTransforms` is every skeleton bone's model-space transform
        // (SkeletonData::ComputeModelTransforms); empty = the rest pose. Attached sub-meshes
        // follow their bones if `attachmentsFollow`, else keep their rest placement. Without a
        // skeleton, the rest palette whatever the arguments. Throws std::invalid_argument if the
        // transforms are not one per bone.
        void ComputePalette(std::span<const glm::mat4> boneTransforms, bool attachmentsFollow,
                            std::vector<glm::mat4>& palette) const;
    };

    // Loads Frost assets (.fmesh, .ftex, .fskel, .fskmesh, .fanim; see AssetFile) and keeps them
    // loaded, one instance per asset: loading the same asset again returns the cached one, even if
    // its file has moved. Owned by Engine; shared by the game and the editor. Source files (glTF,
    // PNG, ...) are not loaded here: they are turned into assets first, by AssetImporter.
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

        // Loads (or returns the cached) skeleton / animation asset. Throws ImportError like LoadMesh.
        [[nodiscard]]
        std::shared_ptr<const SkeletonAsset> LoadSkeleton(const AssetReference& reference);

        [[nodiscard]]
        std::shared_ptr<const SkeletonAsset> LoadSkeleton(const std::filesystem::path& path);

        [[nodiscard]]
        std::shared_ptr<const AnimationAsset> LoadAnimation(const AssetReference& reference);

        [[nodiscard]]
        std::shared_ptr<const AnimationAsset> LoadAnimation(const std::filesystem::path& path);

        // Loads (or returns the cached) skeletal mesh asset, with the skeleton it is bound to.
        // Throws ImportError like LoadMesh. A skeleton or texture that cannot be loaded is reported
        // and left out (see SkeletalMeshAsset) rather than failing the whole mesh.
        [[nodiscard]]
        std::shared_ptr<const SkeletalMeshAsset> LoadSkeletalMesh(const AssetReference& reference);

        [[nodiscard]]
        std::shared_ptr<const SkeletalMeshAsset> LoadSkeletalMesh(const std::filesystem::path& path);


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

        // The type of a Frost asset file, judged by its extension (.fmesh, .ftex, .fskel, .fskmesh,
        // .fanim; case-insensitive); nullopt for anything else (including source files: see
        // AssetImporter::GetSourceType).
        [[nodiscard]]
        static std::optional<AssetType> GetAssetType(const std::filesystem::path& path);

    private:
        // Cache key: the asset id, or the path for version-1 files (which have none).
        [[nodiscard]]
        static std::string CacheKey(const AssetId& id, const std::filesystem::path& path);

        // The file a reference points at now; throws ImportError if it cannot be found.
        [[nodiscard]]
        std::filesystem::path ResolveOrThrow(const AssetReference& reference, AssetType type) const;

        // The materials of a mesh file (version 2 and later), with their textures loaded; a texture
        // that cannot be loaded is reported (as part of `owner`) and counted in `missingTextures`.
        [[nodiscard]]
        std::vector<std::shared_ptr<Material>> CreateMaterials(const std::vector<MaterialAssetData>& materials,
                                                               const std::string& owner, int& missingTextures);

        // Uploads geometry as a MeshAsset: the GPU mesh (with `skin` as its skin stream, if given)
        // with `materials` (per sub-mesh) attached, and the CPU positions (`positions`, or the
        // geometry's own), indices and bounds. The sub-meshes' materialIndex is ignored.
        [[nodiscard]]
        std::shared_ptr<MeshAsset> CreateMeshAsset(const AssetId& id, const std::filesystem::path& path, const MeshData& geometry,
                                                   const std::vector<std::shared_ptr<Material>>& materials,
                                                   std::span<const SkinnedVertex> skin = {},
                                                   std::span<const glm::vec3> positions = {});

        Renderer& m_Renderer;
        AssetRegistry m_Registry;

        std::map<std::string, std::shared_ptr<const MeshAsset>> m_Meshes;
        std::map<std::pair<std::string, bool>, std::shared_ptr<Texture>> m_Textures;  // (key, srgb).
        std::map<std::string, std::shared_ptr<const SkeletonAsset>> m_Skeletons;
        std::map<std::string, std::shared_ptr<const SkeletalMeshAsset>> m_SkeletalMeshes;
        std::map<std::string, std::shared_ptr<const AnimationAsset>> m_Animations;
    };

}  // namespace Frost
