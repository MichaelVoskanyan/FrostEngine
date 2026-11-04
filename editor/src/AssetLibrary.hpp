#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Assets/AssetImporter.hpp>
#include <Frost/Assets/AssetRegistry.hpp>
#include <Frost/Project/Project.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Frost {
    class AssetManager;
}

// The project's assets, as the editor organizes them: every Frost asset file (.fmesh, .ftex) under
// the project's content directory, in whatever folder structure the user likes (e.g.
// Weapons/Glock19/Meshes, Weapons/Glock19/Textures), plus its levels (.flevel, in Levels/ by
// default). The content directory is the library; the engine's AssetRegistry (owned by the
// AssetManager) knows where each asset is. Nothing outside the content directory is part of it.
//
// Every operation here keeps asset ids: renaming or moving an asset or folder is a plain file
// rename, and references (levels, meshes referring to their textures) find the asset by id
// wherever it ends up. The same holds for changes made outside the editor (in the OS file
// manager): Poll() picks them up.
class AssetLibrary final {
public:
    using Entry = Frost::AssetRegistry::Entry;

    // `assets` must be the engine's for `project` (its registry covers the project's content).
    AssetLibrary(Frost::AssetManager& assets, const Frost::Project& project);

    [[nodiscard]]
    const std::filesystem::path& GetContentDirectory() const noexcept;

    // The project's Levels folder (which cannot be renamed, moved or deleted here).
    [[nodiscard]]
    const std::filesystem::path& GetLevelsDirectory() const noexcept;

    [[nodiscard]]
    Frost::AssetRegistry& GetRegistry() noexcept;

    [[nodiscard]]
    const std::vector<Entry>& GetEntries() const noexcept;

    [[nodiscard]]
    const Entry* Find(const Frost::AssetId& id) const;

    [[nodiscard]]
    const Entry* FindByPath(const std::filesystem::path& path) const;

    // Rescans the content directory (assets, folders and levels). Returns whether anything changed.
    bool Refresh();

    // Refresh(), at most once per `interval`; for picking up changes made outside the editor.
    bool Poll(std::chrono::milliseconds interval);

    // Brings assets from before asset ids (version 1) up to date in place; see
    // AssetImporter::UpgradeLegacyAsset. Returns one message per asset upgraded or failed.
    [[nodiscard]]
    std::vector<std::string> UpgradeLegacyAssets();

    // --- Folders (absolute paths inside the content directory) -----------------------------------

    // Direct subfolders, sorted by name.
    [[nodiscard]]
    const std::vector<std::filesystem::path>& GetSubfolders(const std::filesystem::path& folder) const;

    // Assets directly in `folder`, models first, then by name.
    [[nodiscard]]
    std::vector<const Entry*> GetAssetsIn(const std::filesystem::path& folder) const;

    // Level files (.flevel) directly in `folder`, by name.
    [[nodiscard]]
    const std::vector<std::filesystem::path>& GetLevelsIn(const std::filesystem::path& folder) const;

    // Every level file in the content directory.
    [[nodiscard]]
    std::vector<std::filesystem::path> GetAllLevels() const;

    // Whether `folder` (recursively) contains any asset.
    [[nodiscard]]
    bool ContainsAssets(const std::filesystem::path& folder) const;

    // --- Operations. Each returns an error message, empty on success, and rescans. ---------------

    std::string CreateFolder(const std::filesystem::path& parent, const std::string& name,
                             std::filesystem::path* created = nullptr);

    // Renames an asset (its extension is kept) or a folder. Ids are unaffected.
    std::string Rename(const std::filesystem::path& item, const std::string& newName,
                       std::filesystem::path* renamed = nullptr);

    // Moves an asset or folder into `targetFolder`. Ids are unaffected.
    std::string Move(const std::filesystem::path& item, const std::filesystem::path& targetFolder,
                     std::filesystem::path* moved = nullptr);

    // Deletes an asset file. References to it become missing (see FindReferencers).
    std::string DeleteAsset(const std::filesystem::path& asset);

    // Deletes a folder that contains no assets (empty subfolders go with it).
    std::string DeleteFolder(const std::filesystem::path& folder);

    // Copies an asset under a new name in the same folder, with an id of its own.
    std::string Duplicate(const std::filesystem::path& asset, std::filesystem::path* copy = nullptr);

    // Gives an asset whose id duplicates another's (a copy made outside the editor) its own id.
    std::string MakeUnique(const std::filesystem::path& asset);

    // Saved files in the content directory that refer to the asset: levels, and meshes whose
    // materials use it (for a texture).
    [[nodiscard]]
    std::vector<std::filesystem::path> FindReferencers(const Frost::AssetId& id) const;

    // --- Importing -------------------------------------------------------------------------------

    // Imports `source` into `destination` (and, for a model, its textures into `textureFolder`;
    // empty = the default) and rescans. Throws Frost::ImportError.
    Frost::AssetImporter::Result Import(const std::filesystem::path& source, const std::filesystem::path& destination,
                                        const std::filesystem::path& textureFolder = {});

    // The asset imported from `source` earlier, if any.
    [[nodiscard]]
    const Entry* FindBySource(const std::filesystem::path& source) const;

    // Whether `path` is the content directory or inside it.
    [[nodiscard]]
    bool IsInside(const std::filesystem::path& path) const;

private:
    // Rescans folders and level files; returns whether either changed.
    bool RescanFolders();

    Frost::AssetManager& m_Assets;
    std::filesystem::path m_ContentDirectory;
    std::filesystem::path m_LevelsDirectory;
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> m_Subfolders;  // Folder -> children.
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> m_Levels;      // Folder -> level files.
    std::chrono::steady_clock::time_point m_LastPoll{};
};
