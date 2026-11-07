#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Assets/AssetType.hpp>
#include <Frost/Assets/ImportError.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Frost {

    class AssetRegistry;
    struct GltfModelInfo;

    // Turns source files into Frost assets (see AssetFile): the one step where third-party formats
    // are read. An image (.png/.jpg, through ImageImporter) becomes a .ftex. A model (.gltf/.glb,
    // through GltfModel) can become any of:
    //   - a static mesh (.fmesh): the model as posed in the file;
    //   - a skeleton (.fskel): its bone hierarchy;
    //   - a skeletal mesh (.fskmesh): the model bound to a skeleton (the new one, or an existing
    //     skeleton asset with bones of the same names, e.g. one imported from another file of the
    //     same rig);
    //   - one animation (.fanim) per glTF animation chosen, for that skeleton;
    //   - with a skeletal mesh, meshes (.fmesh) of the model's rigid parts chosen to be separate
    //     attachments (a magazine, a muzzle brake): left out of the skeletal mesh, which lists them
    //     as the objects it comes with (see DefaultAttachment), so a level can remove or swap them.
    // Each texture a mesh (static or skeletal) comes with becomes a .ftex of its own, which the
    // mesh's materials refer to (so textures can also be used on their own, e.g. applied to other
    // objects). Assets record what they were imported from (the source file, and for one of several
    // assets made from it, which item: "texture:<name>", "animation:<name>", ...), for reimporting.
    //
    // Reimporting into an existing asset keeps its id, and a model's textures keep theirs (each is
    // matched to the texture of the source it came from), so nothing that refers to them breaks. A
    // reimported skeletal mesh or animation keeps the skeleton it was bound to unless told otherwise.
    // What was authored in the editor is kept too: a skeleton's added bones (those whose parent the
    // model still has), a skeletal mesh's sockets, an animation's visibility keys.
    //
    // Used by tools (the editor); the game only ever loads the resulting assets.
    class AssetImporter final {
    public:
        AssetImporter() = delete;

        // One asset (or a model's static mesh with its textures) from a source file.
        struct Options {
            // The asset to write (replaced, keeping its id, if it exists). Its extension decides
            // what a model is imported as: .fmesh, .fskel, .fskmesh or .fanim.
            std::filesystem::path destination{};
            // Where a mesh's textures go; empty = GetDefaultTextureFolder(destination).
            std::filesystem::path textureFolder{};
            // For finding the textures an earlier import of the same model produced (so they are
            // updated rather than duplicated), and the skeleton a skeletal mesh or animation is
            // bound to. Optional; without it textures are always new and skeletons are found by path.
            const AssetRegistry* registry = nullptr;
            // The project directory paths are stored relative to (see AssetFile); empty = the
            // registry's.
            std::filesystem::path projectDirectory{};
            // A skeletal mesh or animation: the skeleton asset to bind it to. Unset = the one the
            // existing asset at `destination` is bound to, else the skeleton imported earlier from
            // the same source.
            AssetReference skeleton{};
            // An animation: which of the source's animations (see GetAnimationItem). Empty = the one
            // the existing asset at `destination` was imported from.
            std::string sourceItem{};
            // A skeletal mesh: how its rigid parts are attached to bones (see GltfAttachmentOptions).
            // Reimporting keeps the attachments the existing asset has, except for the parts named
            // in `partBones`.
            bool attachLooseParts = true;
            std::map<std::string, std::string> partBones{};
            // A skeletal mesh: the parts (by name) imported as separate attachments (see
            // ModelOptions::separateParts); nullopt = those the existing asset has as such (none
            // for a new one).
            std::optional<std::set<std::string>> separateParts{};
        };

        struct Result {
            AssetReference asset;
            std::vector<AssetReference> textures;  // A mesh's textures, as written.
            // Problems that did not stop the import (e.g. animation channels for nodes that are not
            // bones of the skeleton, which were left out).
            std::vector<std::string> warnings;
            // A skeletal mesh's separate parts' meshes, as written (see ModelOptions::separateParts).
            std::vector<AssetReference> parts;
        };

        // Several assets from one model file at once (it is read once). Every destination is
        // optional; at least one must be given.
        struct ModelOptions {
            struct Animation {
                std::size_t index = 0;               // Into GltfModelInfo::animations.
                std::filesystem::path destination{};  // .fanim
            };

            std::filesystem::path mesh{};          // A static mesh (.fmesh).
            std::filesystem::path skeletalMesh{};  // A skeletal mesh (.fskmesh).
            // The skeleton the skeletal mesh and animations are bound to: a new one made from the
            // file, written here (.fskel)...
            std::filesystem::path skeleton{};
            // ...or, if `skeleton` is empty, this existing skeleton asset (matched by bone name).
            AssetReference existingSkeleton{};
            std::vector<Animation> animations{};
            // How the skeletal mesh's rigid parts are attached to bones (see GltfAttachmentOptions):
            // loose parts to a suggested bone (or not at all), and per part (by name) a bone of
            // choice ("" = none).
            bool attachLooseParts = true;
            std::map<std::string, std::string> partBones{};
            // Rigid parts (by name) of the skeletal mesh to import as meshes of their own instead,
            // to be attached to it as separate objects: each is written next to the skeletal mesh
            // as "<skeletal mesh>_<part>.fmesh" (or where an earlier import of the part went),
            // modeled in the skeletal mesh's space, and listed in the skeletal mesh as one of its
            // default attachments, moving with the bone it would have been attached to (see
            // GltfSeparatePart). The editor places those with the skeletal mesh.
            std::set<std::string> separateParts{};

            // As in Options: where the meshes' textures go (empty = GetDefaultTextureFolder of the
            // skeletal mesh, else of the static mesh), and the registry / project directory.
            std::filesystem::path textureFolder{};
            const AssetRegistry* registry = nullptr;
            std::filesystem::path projectDirectory{};
        };

        struct ModelResult {
            AssetReference mesh;
            AssetReference skeletalMesh;
            AssetReference skeleton;                 // The one written, or the existing one used.
            std::vector<AssetReference> animations;  // In ModelOptions::animations order.
            std::vector<AssetReference> parts;       // The separate parts' meshes, as written.
            std::vector<AssetReference> textures;
            std::vector<std::string> warnings;
        };

        // What a source file imports as, judged by its extension (case-insensitive): Mesh for a
        // model (which can also be imported as the other model types, see CanImportAs), Texture for
        // an image; nullopt if no importer handles it.
        [[nodiscard]]
        static std::optional<AssetType> GetSourceType(const std::filesystem::path& source);

        // Whether `source` is a model (.gltf/.glb).
        [[nodiscard]]
        static bool IsModelSource(const std::filesystem::path& source);

        // Whether `source` can be imported as an asset of `type` (a model as a mesh, skeleton,
        // skeletal mesh or animation; an image as a texture).
        [[nodiscard]]
        static bool CanImportAs(const std::filesystem::path& source, AssetType type);

        // The asset file extension for a type (".fmesh", ".ftex", ".fskel", ".fskmesh", ".fanim").
        [[nodiscard]]
        static const char* GetAssetExtension(AssetType type) noexcept;

        // Where a model's textures go by default: the "Textures" folder next to the mesh's folder
        // if the mesh is in a folder called "Meshes" (Weapons/Glock19/Meshes ->
        // Weapons/Glock19/Textures), else a "Textures" folder inside the mesh's folder.
        [[nodiscard]]
        static std::filesystem::path GetDefaultTextureFolder(const std::filesystem::path& meshPath);

        // Where a model's animations go by default: likewise, an "Animations" folder.
        [[nodiscard]]
        static std::filesystem::path GetDefaultAnimationFolder(const std::filesystem::path& meshPath);

        // Identifies animation `index` of a model within it, stably across reimports (the source
        // item its asset records): "animation:<name>" when it has a unique name, else
        // "animation#<index>".
        [[nodiscard]]
        static std::string GetAnimationItem(const GltfModelInfo& info, std::size_t index);

        // The animation a source item identifies, if the model (still) has it.
        [[nodiscard]]
        static std::optional<std::size_t> FindAnimation(const GltfModelInfo& info, std::string_view item);

        // How a rigid part of a skeletal mesh was imported: the bone it moves with ("" = none), and
        // whether it is a separate attachment rather than part of the mesh.
        struct PartAttachment {
            std::string bone;
            bool separate = false;
        };

        // How each rigid part (by name) of an existing skeletal mesh asset was imported, as
        // GltfAttachmentOptions::partBones and separateParts; nullopt if the file cannot be read or
        // predates attachments (version 1). For keeping the choices when reimporting.
        [[nodiscard]]
        static std::optional<std::map<std::string, PartAttachment>> GetPartAttachments(const std::filesystem::path& skeletalMesh,
                                                                                        const std::filesystem::path& projectDirectory);

        // The source item a separate part's mesh records: "part:<name>".
        [[nodiscard]]
        static std::string GetPartItem(std::string_view partName);

        // Reads `source` and writes the asset(s). Throws ImportError if the source is unsupported,
        // unreadable or malformed, cannot be imported as the destination's type, a skeletal mesh or
        // animation has no skeleton to bind to, or an asset cannot be written.
        static Result Import(const std::filesystem::path& source, const Options& options);

        // Reads a model and writes the chosen assets. Everything is imported before anything is
        // written, so a problem with any part (which throws ImportError, as Import) leaves the
        // project as it was.
        static ModelResult ImportModel(const std::filesystem::path& source, const ModelOptions& options);

        // Brings a version-1 asset (from before ids) up to date in place: it gets an id, and a
        // mesh's embedded textures become .ftex assets in its default texture folder. The project
        // directory is the registry's. Throws ImportError.
        static Result UpgradeLegacyAsset(const std::filesystem::path& asset, const AssetRegistry* registry);
    };

}  // namespace Frost
