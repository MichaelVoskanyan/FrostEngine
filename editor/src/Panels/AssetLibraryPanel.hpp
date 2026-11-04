#pragma once

#include "../AssetLibrary.hpp"
#include "../Widgets/FileBrowser.hpp"

#include <filesystem>
#include <functional>
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
//     a second. Drag models into the viewport to place them, and textures onto a material's texture
//     slot in the Inspector.
//   - Import...: turns a source file (a .gltf/.glb model, whose textures become separate texture
//     assets, or a .png/.jpg image) into assets, into the current folder by default.
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
    std::string m_ImportFolder;                         // Content-relative folder for the asset.
    std::string m_ImportTextureFolder;                  // Content-relative folder for a model's textures.
    bool m_TextureFolderEdited = false;                 // Stop following m_ImportFolder once edited.
    std::string m_ImportError;
};
