#pragma once

#include "../AssetLibrary.hpp"
#include "../Widgets/FileBrowser.hpp"

#include <Frost/Assets/GltfImporter.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class EditorContext;

// The "Asset Library" window: the project's content browser.
//   - Library tab: the content directory as a folder tree (left) and the current folder's
//     subfolders and assets (right). Organize freely: create, rename, move (drag onto a folder in
//     the tree, the list or the path bar) and delete folders; rename, move, duplicate and delete
//     assets. Nothing that refers to an asset breaks when it is renamed or moved (assets are
//     referred to by id), including moves made in the OS file manager, which show up here within
//     a second. Drag models and skeletal meshes into the viewport to place them, textures onto a
//     material's texture slot and animations onto a skeletal mesh's animation slot in the
//     Inspector. Double-click a skeletal mesh or skeleton (or right-click > Edit...) to open its
//     editor (see SkeletalAssetEditor).
//   - Import...: turns a source file into assets, into the current folder by default: an image
//     (.png/.jpg) into a texture, a model (.gltf/.glb) into any of a static mesh, a skeletal mesh,
//     its skeleton (or the skeletal mesh and animations bound to an existing skeleton) and each of
//     its animations, chosen one by one. A mesh's textures become separate texture assets.
//   - File System tab: browse the disk; double-click a source file to import it.
//   - Lights tab: directional, point and spot lights, to drag into the viewport.
class AssetLibraryPanel final {
public:
    static constexpr const char* kWindowName = "Asset Library";

    explicit AssetLibraryPanel(const std::filesystem::path& startDirectory);

    void Draw(EditorContext& context);

private:
    // A modal asking for a name (new folder, rename).
    struct NamePrompt {
        enum class Kind { NewFolder, Rename };
        Kind kind = Kind::NewFolder;
        std::filesystem::path target;  // Parent folder (NewFolder) or the item to rename.
        std::string name;
        std::string error;
    };

    // A modal confirming a deletion.
    struct DeletePrompt {
        std::filesystem::path target;
        bool isFolder = false;
        std::vector<std::filesystem::path> referencers;  // Saved files that refer to the asset.
        int usesInOpenLevel = 0;                          // Objects in the open level using it.
        std::string error;
    };

    void DrawLibraryTab(EditorContext& context);
    void DrawFolderTree(EditorContext& context, const std::filesystem::path& folder);
    void DrawBreadcrumbs(EditorContext& context);
    void DrawFolderContents(EditorContext& context);
    void DrawFolderRow(EditorContext& context, const std::filesystem::path& folder);
    void DrawAssetRow(EditorContext& context, const AssetLibrary::Entry& entry, bool showFolder);
    void DrawLevelRow(EditorContext& context, const std::filesystem::path& level, bool showFolder);
    void DrawFolderMenu(EditorContext& context, const std::filesystem::path& folder);
    void DrawFileSystemTab(EditorContext& context);
    void DrawLightsTab(EditorContext& context);

    void DrawNamePrompt(EditorContext& context);
    void DrawDeletePrompt(EditorContext& context);
    void DrawImportDialog(EditorContext& context);

    // What the import dialog would do: why it cannot (yet), or how to run the import (returning
    // an error message, empty on success) and which folder to show afterwards.
    struct ImportPlan {
        std::string problem;
        std::function<std::string()> run;
        std::filesystem::path folder;
    };

    // The import dialog's options for a model (what to import it as) and for a single asset (an
    // image, or a fixed destination). `submit` is set when a field asks for the import (Enter).
    [[nodiscard]]
    ImportPlan DrawModelImportOptions(EditorContext& context, const std::filesystem::path& source, bool& submit);
    [[nodiscard]]
    ImportPlan DrawSingleImportOptions(EditorContext& context, const std::filesystem::path& source, bool& submit);

    // Reads what a model contains into m_Inspection, unless it already holds that version of it.
    void InspectModel(const std::filesystem::path& source);

    // Fills in the model import options for `source` (what to import, where, and which existing
    // assets an earlier import of it made).
    void SuggestModelOutputs(EditorContext& context, const std::filesystem::path& source);

    // Makes `folder` (and the item dropped on it) a move target.
    void AcceptMoveInto(EditorContext& context, const std::filesystem::path& folder);

    void RequestNewFolder(const std::filesystem::path& parent);
    void RequestRename(const std::filesystem::path& item);
    void RequestDelete(EditorContext& context, const std::filesystem::path& item);

    // Opens the import dialog for `source` (may be empty), into the current folder; with
    // `destination`, the asset to reimport into is fixed ("Reimport From...").
    void OpenImportDialog(EditorContext& context, const std::filesystem::path& source,
                          const std::optional<std::filesystem::path>& destination = std::nullopt);
    void SuggestImportDestination(EditorContext& context);

    // Content-relative display of a folder ("Weapons/Glock19"; "Content" for the root).
    [[nodiscard]]
    std::string DisplayFolder(EditorContext& context, const std::filesystem::path& folder) const;

    // Operations requested while drawing (moves, deletions, ...) change the library, which would
    // invalidate the lists being drawn; they run after the window is done.
    std::vector<std::function<void()>> m_Deferred;

    std::filesystem::path m_CurrentFolder;  // Empty until the first Draw (then the content directory).
    std::string m_Filter;
    FileBrowser m_FileSystem;

    std::optional<NamePrompt> m_NamePrompt;
    bool m_OpenNamePrompt = false;
    std::optional<DeletePrompt> m_DeletePrompt;
    bool m_OpenDeletePrompt = false;

    // Import dialog state.
    bool m_OpenImportDialog = false;
    FileBrowser m_ImportBrowser;
    std::string m_ImportSource;                         // UTF-8 path as typed/picked.
    std::string m_SuggestedForSource;                   // The source the destination was last suggested for.
    std::optional<std::filesystem::path> m_FixedDestination;  // Reimport From...: the asset to replace.
    std::string m_ImportName;                           // Asset name, without extension.
    std::string m_ImportFolder;                         // Content-relative folder for the asset(s).
    std::string m_ImportTextureFolder;                  // Content-relative folder for a model's textures.
    bool m_TextureFolderEdited = false;                 // Stop following m_ImportFolder once edited.
    std::string m_ImportError;

    // What the model being imported contains (read once per file version).
    struct ModelInspection {
        std::filesystem::path source;  // Normalized; empty = nothing inspected.
        std::filesystem::file_time_type modified{};
        std::uintmax_t size = 0;
        std::optional<Frost::GltfModelInfo> info;
        std::string error;  // Why it cannot be read.
    };
    ModelInspection m_Inspection;

    // One asset a model can become: whether to import it, and as what. Importing updates the asset
    // an earlier import of the same source made (`existing`, wherever it is now) while the name is
    // that asset's; another name makes a new asset in the folder.
    struct ModelOutput {
        bool enabled = false;
        std::string name;
        std::optional<std::filesystem::path> existing;
    };
    ModelOutput m_StaticMesh;
    ModelOutput m_SkeletalMesh;
    ModelOutput m_Skeleton;            // When importing the model's own skeleton.
    bool m_UseExistingSkeleton = false;
    Frost::AssetReference m_ExistingSkeleton;  // Otherwise: bind to this skeleton asset.
    std::vector<ModelOutput> m_Animations;     // By animation index.
    // How the skeletal mesh's rigid parts are attached to bones (see GltfAttachmentOptions): loose
    // parts to their suggested bones or not, and the bones chosen per part (by name; seeded from
    // the skeletal mesh an earlier import made, so reimporting keeps them).
    bool m_AttachLooseParts = true;
    std::map<std::string, std::string> m_PartBones;
    // Per part (by name), whether it is imported as a separate object; absent = the default (loose
    // parts are).
    std::map<std::string, bool> m_SeparateParts;
    std::map<std::string, std::string> m_PreviousPartBones;  // What the earlier import chose (for labels).
    std::string m_AnimationFolder;             // Content-relative folder for new animations.
    bool m_AnimationFolderEdited = false;
    std::size_t m_FixedAnimation = 0;          // Reimport From... an animation: which of the source's.
};
