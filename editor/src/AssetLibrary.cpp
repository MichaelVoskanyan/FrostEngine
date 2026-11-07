#include "AssetLibrary.hpp"

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

    [[nodiscard]]
    std::string Lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // A plain file/folder name: no separators, nothing reserved.
    [[nodiscard]]
    bool IsValidName(const std::string& name) {
        if (name.empty() || name == "." || name == ".." || name.back() == ' ' || name.back() == '.') { return false; }
        return std::none_of(name.begin(), name.end(), [](const char c) {
            return static_cast<unsigned char>(c) < 0x20 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos;
        });
    }

    [[nodiscard]]
    bool Exists(const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::exists(path, error);
    }

    // Renames `from` to `to`, refusing to replace something else (std::filesystem::rename would
    // silently replace a file on most platforms). A change of letter case only, on a
    // case-insensitive file system, goes through a temporary name.
    [[nodiscard]]
    std::string SafeRename(const std::filesystem::path& from, const std::filesystem::path& to) {
        std::error_code error;
        if (Exists(to)) {
            if (!std::filesystem::equivalent(from, to, error) || from == to) {
                return "'" + Frost::PathToUtf8(to.filename()) + "' already exists there.";
            }

            std::filesystem::path temporary = from;
            temporary += ".renaming";
            std::filesystem::rename(from, temporary, error);
            if (!error) { std::filesystem::rename(temporary, to, error); }
        }
        else {
            std::filesystem::rename(from, to, error);
        }
        return error ? "Could not rename '" + Frost::PathToUtf8(from.filename()) + "': " + error.message() : std::string();
    }

    [[nodiscard]]
    std::string ReadTextFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

}  // namespace

AssetLibrary::AssetLibrary(Frost::AssetManager& assets, const Frost::Project& project)
    : m_Assets(assets),
      m_ContentDirectory(assets.GetRegistry().GetRoot()),
      m_LevelsDirectory(Frost::AssetManager::NormalizePath(project.GetLevelsDirectory())) {
    std::error_code error;
    std::filesystem::create_directories(m_ContentDirectory, error);
    if (error) {
        std::cerr << "[Editor] Could not create the content directory '" << Frost::PathToUtf8(m_ContentDirectory)
                  << "': " << error.message() << '\n';
    }
    Refresh();
}

const std::filesystem::path& AssetLibrary::GetContentDirectory() const noexcept { return m_ContentDirectory; }

const std::filesystem::path& AssetLibrary::GetLevelsDirectory() const noexcept { return m_LevelsDirectory; }

Frost::AssetRegistry& AssetLibrary::GetRegistry() noexcept { return m_Assets.GetRegistry(); }

const std::vector<AssetLibrary::Entry>& AssetLibrary::GetEntries() const noexcept {
    return m_Assets.GetRegistry().GetEntries();
}

const AssetLibrary::Entry* AssetLibrary::Find(const Frost::AssetId& id) const { return m_Assets.GetRegistry().Find(id); }

const AssetLibrary::Entry* AssetLibrary::FindByPath(const std::filesystem::path& path) const {
    return m_Assets.GetRegistry().FindByPath(path);
}

bool AssetLibrary::Refresh() {
    const bool assetsChanged = m_Assets.GetRegistry().Refresh();
    const bool foldersChanged = RescanFolders();
    m_LastPoll = std::chrono::steady_clock::now();
    return assetsChanged || foldersChanged;
}

bool AssetLibrary::Poll(const std::chrono::milliseconds interval) {
    if (std::chrono::steady_clock::now() - m_LastPoll < interval) { return false; }
    return Refresh();
}

std::vector<std::string> AssetLibrary::UpgradeLegacyAssets() {
    std::vector<std::filesystem::path> legacy;
    for (const Entry& entry : GetEntries()) {
        if (!Frost::AssetFile::HasId(entry.type, entry.version)) { legacy.push_back(entry.path); }
    }

    std::vector<std::string> messages;
    for (const std::filesystem::path& path : legacy) {
        try {
            const Frost::AssetImporter::Result result = Frost::AssetImporter::UpgradeLegacyAsset(path, &GetRegistry());
            std::string message = "Upgraded '" + Frost::PathToUtf8(path.filename()) + "' to the current asset format";
            if (!result.textures.empty()) {
                message += "; its " + std::to_string(result.textures.size()) + " embedded texture(s) are now separate assets";
            }
            messages.push_back(message + ".");
        }
        catch (const Frost::ImportError& e) {
            messages.push_back(std::string("Could not upgrade an asset: ") + e.what());
        }
    }

    if (!legacy.empty()) { Refresh(); }
    return messages;
}

const std::vector<std::filesystem::path>& AssetLibrary::GetSubfolders(const std::filesystem::path& folder) const {
    static const std::vector<std::filesystem::path> kNone;
    const auto found = m_Subfolders.find(Frost::AssetManager::NormalizePath(folder));
    return found != m_Subfolders.end() ? found->second : kNone;
}

std::vector<const AssetLibrary::Entry*> AssetLibrary::GetAssetsIn(const std::filesystem::path& folder) const {
    const std::filesystem::path normalized = Frost::AssetManager::NormalizePath(folder);

    std::vector<const Entry*> assets;
    for (const Entry& entry : GetEntries()) {
        if (entry.path.parent_path() == normalized) { assets.push_back(&entry); }
    }

    // Types in the order tools list them (Frost::kAssetTypes).
    const auto typeOrder = [](const Frost::AssetType type) {
        return std::find(std::begin(Frost::kAssetTypes), std::end(Frost::kAssetTypes), type) - std::begin(Frost::kAssetTypes);
    };
    std::sort(assets.begin(), assets.end(), [&](const Entry* a, const Entry* b) {
        if (a->type != b->type) { return typeOrder(a->type) < typeOrder(b->type); }
        return Lowercase(Frost::PathToUtf8(a->path.filename())) < Lowercase(Frost::PathToUtf8(b->path.filename()));
    });
    return assets;
}

const std::vector<std::filesystem::path>& AssetLibrary::GetLevelsIn(const std::filesystem::path& folder) const {
    static const std::vector<std::filesystem::path> kNone;
    const auto found = m_Levels.find(Frost::AssetManager::NormalizePath(folder));
    return found != m_Levels.end() ? found->second : kNone;
}

std::vector<std::filesystem::path> AssetLibrary::GetAllLevels() const {
    std::vector<std::filesystem::path> all;
    for (const auto& [folder, levels] : m_Levels) { all.insert(all.end(), levels.begin(), levels.end()); }
    return all;
}

bool AssetLibrary::ContainsAssets(const std::filesystem::path& folder) const {
    const std::filesystem::path normalized = Frost::AssetManager::NormalizePath(folder);
    return std::any_of(GetEntries().begin(), GetEntries().end(), [&](const Entry& entry) {
        const std::filesystem::path relative = entry.path.lexically_relative(normalized);
        return !relative.empty() && *relative.begin() != "..";
    });
}

std::string AssetLibrary::CreateFolder(const std::filesystem::path& parent, const std::string& name,
                                       std::filesystem::path* const created) {
    if (!IsValidName(name)) { return "'" + name + "' is not a valid folder name."; }
    if (!IsInside(parent)) { return "Folders can only be created inside the content directory."; }

    const std::filesystem::path folder = Frost::AssetManager::NormalizePath(parent / Frost::PathFromUtf8(name));
    if (Exists(folder)) { return "'" + name + "' already exists there."; }

    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) { return "Could not create the folder: " + error.message(); }

    if (created != nullptr) { *created = folder; }
    Refresh();
    return {};
}

std::string AssetLibrary::Rename(const std::filesystem::path& item, const std::string& newName,
                                 std::filesystem::path* const renamed) {
    const std::filesystem::path from = Frost::AssetManager::NormalizePath(item);
    if (!IsInside(from) || from == m_ContentDirectory) { return "Only items inside the content directory can be renamed."; }
    if (from == m_LevelsDirectory) { return "The Levels folder is where the project's levels go; it cannot be renamed."; }
    if (!IsValidName(newName)) { return "'" + newName + "' is not a valid name."; }

    std::error_code error;
    const bool isFolder = std::filesystem::is_directory(from, error);

    // Assets keep their extension (it says what they are); typing it is harmless.
    std::filesystem::path name = Frost::PathFromUtf8(newName);
    if (!isFolder && Lowercase(Frost::PathToUtf8(name.extension())) != Lowercase(Frost::PathToUtf8(from.extension()))) {
        name += from.extension();
    }

    const std::filesystem::path to = from.parent_path() / name;
    if (const std::string failure = SafeRename(from, to); !failure.empty()) { return failure; }

    if (renamed != nullptr) { *renamed = Frost::AssetManager::NormalizePath(to); }
    Refresh();
    return {};
}

std::string AssetLibrary::Move(const std::filesystem::path& item, const std::filesystem::path& targetFolder,
                               std::filesystem::path* const moved) {
    const std::filesystem::path from = Frost::AssetManager::NormalizePath(item);
    const std::filesystem::path target = Frost::AssetManager::NormalizePath(targetFolder);

    if (!IsInside(from) || from == m_ContentDirectory) { return "Only items inside the content directory can be moved."; }
    if (from == m_LevelsDirectory) { return "The Levels folder is where the project's levels go; it cannot be moved."; }
    if (!IsInside(target)) { return "Items can only be moved within the content directory."; }
    if (from.parent_path() == target) { return {}; }  // Already there.

    // A folder cannot go into itself or one of its own subfolders.
    const std::filesystem::path relative = target.lexically_relative(from);
    if (!relative.empty() && *relative.begin() != "..") { return "A folder cannot be moved into itself."; }

    const std::filesystem::path to = target / from.filename();
    if (const std::string failure = SafeRename(from, to); !failure.empty()) { return failure; }

    if (moved != nullptr) { *moved = Frost::AssetManager::NormalizePath(to); }
    Refresh();
    return {};
}

std::string AssetLibrary::DeleteAsset(const std::filesystem::path& asset) {
    const std::filesystem::path path = Frost::AssetManager::NormalizePath(asset);
    if (!IsInside(path) || !Frost::AssetManager::GetAssetType(path).has_value()) {
        return "Only assets inside the content directory can be deleted.";
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) { return "Could not delete '" + Frost::PathToUtf8(path.filename()) + "': " + error.message(); }

    Refresh();
    return {};
}

std::string AssetLibrary::DeleteFolder(const std::filesystem::path& folder) {
    const std::filesystem::path path = Frost::AssetManager::NormalizePath(folder);
    if (!IsInside(path) || path == m_ContentDirectory) { return "Only folders inside the content directory can be deleted."; }
    if (path == m_LevelsDirectory) { return "The Levels folder is where the project's levels go; it cannot be deleted."; }
    if (ContainsAssets(path)) { return "The folder still contains assets; move or delete them first."; }

    // Anything that is not an asset (notes, levels, stray files) is not the library's to delete.
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_directory(error)) {
            return "The folder contains other files (e.g. '" + Frost::PathToUtf8(it->path().filename()) +
                   "'); remove them first.";
        }
    }

    std::filesystem::remove_all(path, error);
    if (error) { return "Could not delete the folder: " + error.message(); }

    Refresh();
    return {};
}

std::string AssetLibrary::Duplicate(const std::filesystem::path& asset, std::filesystem::path* const copy) {
    const std::filesystem::path from = Frost::AssetManager::NormalizePath(asset);

    std::filesystem::path to;
    for (int suffix = 2;; ++suffix) {
        to = from.parent_path() / from.stem();
        to += "_" + std::to_string(suffix);
        to += from.extension();
        if (!Exists(to)) { break; }
    }

    std::error_code error;
    std::filesystem::copy_file(from, to, error);
    if (error) { return "Could not copy '" + Frost::PathToUtf8(from.filename()) + "': " + error.message(); }

    // A copy is a different asset: it must not answer to the original's id.
    try {
        Frost::AssetFile::RewriteId(to, Frost::AssetId::Generate());
    }
    catch (const Frost::ImportError& e) {
        std::filesystem::remove(to, error);
        return e.what();
    }

    if (copy != nullptr) { *copy = Frost::AssetManager::NormalizePath(to); }
    Refresh();
    return {};
}

std::string AssetLibrary::MakeUnique(const std::filesystem::path& asset) {
    try {
        Frost::AssetFile::RewriteId(asset, Frost::AssetId::Generate());
    }
    catch (const Frost::ImportError& e) {
        return e.what();
    }
    Refresh();
    return {};
}

std::vector<std::filesystem::path> AssetLibrary::FindReferencers(const Frost::AssetId& id) const {
    std::vector<std::filesystem::path> referencers;
    if (!id.IsValid()) { return referencers; }

    // Levels store ids as hex text; binary assets (meshes' materials, skeletal meshes and
    // animations referring to their skeleton) store them as 16 raw bytes (high, then low, little-
    // endian). Both are searched for as-is: a 128-bit random id does not occur by accident.
    const std::string text = id.ToString();
    std::string bytes(16, '\0');
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<char>((id.high >> (8 * i)) & 0xFF);
        bytes[static_cast<std::size_t>(8 + i)] = static_cast<char>((id.low >> (8 * i)) & 0xFF);
    }

    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(m_ContentDirectory,
                                                          std::filesystem::directory_options::skip_permission_denied, error),
         end;
         !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError)) { continue; }

        const std::string extension = Lowercase(Frost::PathToUtf8(it->path().extension()));
        const bool isLevel = extension == Frost::Project::kLevelExtension;
        const std::optional<Frost::AssetType> type = Frost::AssetManager::GetAssetType(it->path());
        const bool refersToAssets =
                type == Frost::AssetType::Mesh || type == Frost::AssetType::SkeletalMesh || type == Frost::AssetType::Animation;
        if (!isLevel && !refersToAssets) { continue; }

        const std::string contents = ReadTextFile(it->path());
        // An asset's own id is in its header (offset 8); only a match past that is a reference.
        const std::size_t found = contents.find(isLevel ? text : bytes, isLevel ? 0 : 24);
        if (found != std::string::npos) { referencers.push_back(Frost::AssetManager::NormalizePath(it->path())); }
    }

    std::sort(referencers.begin(), referencers.end());
    return referencers;
}

Frost::AssetImporter::Result AssetLibrary::Import(const std::filesystem::path& source,
                                                  const std::filesystem::path& destination,
                                                  const std::filesystem::path& textureFolder,
                                                  Frost::AssetImporter::Options options) {
    // Current first: the importer looks up the textures (and skeletons) an earlier import produced.
    m_Assets.GetRegistry().Refresh();

    options.destination = destination;
    options.textureFolder = textureFolder;
    options.registry = &m_Assets.GetRegistry();
    Frost::AssetImporter::Result result = Frost::AssetImporter::Import(source, options);
    Refresh();
    return result;
}

Frost::AssetImporter::ModelResult AssetLibrary::ImportModel(const std::filesystem::path& source,
                                                            Frost::AssetImporter::ModelOptions options) {
    m_Assets.GetRegistry().Refresh();

    options.registry = &m_Assets.GetRegistry();
    Frost::AssetImporter::ModelResult result = Frost::AssetImporter::ImportModel(source, options);
    Refresh();
    return result;
}

const AssetLibrary::Entry* AssetLibrary::FindBySource(const std::filesystem::path& source) const {
    const std::optional<Frost::AssetType> type = Frost::AssetImporter::GetSourceType(source);
    return type.has_value() ? m_Assets.GetRegistry().FindBySource(source, "", *type) : nullptr;
}

const AssetLibrary::Entry* AssetLibrary::FindBySource(const std::filesystem::path& source, const Frost::AssetType type,
                                                      const std::string_view item) const {
    return m_Assets.GetRegistry().FindBySource(source, item, type);
}

bool AssetLibrary::IsInside(const std::filesystem::path& path) const {
    const std::filesystem::path normalized = Frost::AssetManager::NormalizePath(path);
    if (normalized == m_ContentDirectory) { return true; }
    const std::filesystem::path relative = normalized.lexically_relative(m_ContentDirectory);
    return !relative.empty() && *relative.begin() != "..";
}

bool AssetLibrary::RescanFolders() {
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> folders;
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> levels;
    folders[m_ContentDirectory];

    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(m_ContentDirectory,
                                                          std::filesystem::directory_options::skip_permission_denied, error),
         end;
         !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (it->is_directory(entryError)) {
            const std::filesystem::path path = Frost::AssetManager::NormalizePath(it->path());
            folders[path.parent_path()].push_back(path);
            folders[path];
        }
        else if (Lowercase(Frost::PathToUtf8(it->path().extension())) == Frost::Project::kLevelExtension) {
            const std::filesystem::path path = Frost::AssetManager::NormalizePath(it->path());
            levels[path.parent_path()].push_back(path);
        }
    }

    const auto byName = [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return Lowercase(Frost::PathToUtf8(a.filename())) < Lowercase(Frost::PathToUtf8(b.filename()));
    };
    for (auto& [folder, children] : folders) { std::sort(children.begin(), children.end(), byName); }
    for (auto& [folder, files] : levels) { std::sort(files.begin(), files.end(), byName); }

    const bool changed = folders != m_Subfolders || levels != m_Levels;
    m_Subfolders = std::move(folders);
    m_Levels = std::move(levels);
    return changed;
}
