#include <Frost/Assets/AssetRegistry.hpp>

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace Frost {

    AssetRegistry::AssetRegistry(std::filesystem::path root) { SetRoot(std::move(root)); }

    void AssetRegistry::SetRoot(std::filesystem::path root) {
        m_Root = root.empty() ? std::filesystem::path{} : AssetManager::NormalizePath(root);
        m_Entries.clear();
        Rebuild();
        Refresh();
    }

    const std::filesystem::path& AssetRegistry::GetRoot() const noexcept { return m_Root; }

    std::filesystem::path AssetRegistry::GetProjectDirectory() const { return m_Root.empty() ? m_Root : m_Root.parent_path(); }

    bool AssetRegistry::Refresh() {
        if (m_Root.empty()) { return false; }

        // Previous entries by path: an unchanged file (same size and time) is not read again.
        std::map<std::filesystem::path, Entry> previous;
        for (Entry& entry : m_Entries) { previous.emplace(entry.path, std::move(entry)); }

        std::vector<Entry> entries;
        bool changed = false;

        std::error_code error;
        std::filesystem::recursive_directory_iterator it(m_Root, std::filesystem::directory_options::skip_permission_denied,
                                                         error);
        for (const std::filesystem::recursive_directory_iterator end{}; !error && it != end; it.increment(error)) {
            std::error_code entryError;
            if (!it->is_regular_file(entryError) || !AssetManager::GetAssetType(it->path()).has_value()) { continue; }

            const std::filesystem::path path = AssetManager::NormalizePath(it->path());
            const std::uintmax_t size = it->file_size(entryError);
            const std::filesystem::file_time_type modified = it->last_write_time(entryError);

            if (const auto found = previous.find(path);
                found != previous.end() && found->second.size == size && found->second.modified == modified) {
                entries.push_back(std::move(found->second));
                previous.erase(found);
                continue;
            }

            // New or modified. Anything with an asset extension that is not actually an asset
            // (e.g. a half-written file) is skipped, and picked up by a later scan.
            changed = true;
            const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, GetProjectDirectory());
            if (!header.has_value()) { continue; }

            entries.push_back(Entry{.id = header->id,
                                    .type = header->type,
                                    .version = header->version,
                                    .path = path,
                                    .sourcePath = header->sourcePath,
                                    .sourceItem = header->sourceItem,
                                    .size = size,
                                    .modified = modified,
                                    .duplicate = false});
        }

        // Whatever is left in `previous` was removed (or moved: it reappears under its new path).
        changed = changed || !previous.empty();

        // Always rebuilt: the scan order need not match the previous one, and the lookups index
        // into m_Entries.
        m_Entries = std::move(entries);
        Rebuild();
        if (changed) { ++m_Generation; }
        return changed;
    }

    const std::vector<AssetRegistry::Entry>& AssetRegistry::GetEntries() const noexcept { return m_Entries; }

    std::uint64_t AssetRegistry::GetGeneration() const noexcept { return m_Generation; }

    const AssetRegistry::Entry* AssetRegistry::Find(const AssetId& id) const {
        if (!id.IsValid()) { return nullptr; }
        const auto found = m_ById.find(id);
        return found != m_ById.end() ? &m_Entries[found->second] : nullptr;
    }

    const AssetRegistry::Entry* AssetRegistry::FindByPath(const std::filesystem::path& path) const {
        const auto found = m_ByPath.find(AssetManager::NormalizePath(path));
        return found != m_ByPath.end() ? &m_Entries[found->second] : nullptr;
    }

    const AssetRegistry::Entry* AssetRegistry::FindBySource(const std::filesystem::path& source,
                                                            const std::string_view item, const AssetType type) const {
        const std::filesystem::path normalized = AssetManager::NormalizePath(source);
        for (const Entry& entry : m_Entries) {
            if (!entry.duplicate && entry.type == type && entry.sourceItem == item && entry.sourcePath == normalized) {
                return &entry;
            }
        }
        return nullptr;
    }

    std::optional<std::filesystem::path> AssetRegistry::Resolve(const AssetReference& reference) const {
        if (const Entry* const entry = Find(reference.id); entry != nullptr) { return entry->path; }
        if (reference.path.empty()) { return std::nullopt; }

        // A project only uses its own content: a path outside the content directory (e.g. into
        // another project) is not followed, even if there is an asset there.
        if (!m_Root.empty() && !Contains(reference.path)) { return std::nullopt; }

        // Not in the registry (not scanned yet, or a version-1 file without an id): the path, if
        // it really is that asset. A file at the old path with a different id is some other asset
        // that took its place, not the one referred to.
        if (const Entry* const entry = FindByPath(reference.path); entry != nullptr) {
            if (!reference.id.IsValid() || !entry->id.IsValid() || entry->id == reference.id) { return entry->path; }
            return std::nullopt;
        }

        const std::optional<AssetHeader> header = AssetFile::ReadHeader(reference.path, GetProjectDirectory());
        if (!header.has_value()) { return std::nullopt; }
        if (reference.id.IsValid() && header->id.IsValid() && header->id != reference.id) { return std::nullopt; }
        return AssetManager::NormalizePath(reference.path);
    }

    bool AssetRegistry::Contains(const std::filesystem::path& path) const {
        if (m_Root.empty()) { return false; }
        const std::filesystem::path relative = AssetManager::NormalizePath(path).lexically_relative(m_Root);
        return !relative.empty() && *relative.begin() != "..";
    }

    void AssetRegistry::Rebuild() {
        std::sort(m_Entries.begin(), m_Entries.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });

        m_ById.clear();
        m_ByPath.clear();
        for (std::size_t i = 0; i < m_Entries.size(); ++i) {
            Entry& entry = m_Entries[i];
            m_ByPath.emplace(entry.path, i);

            // The first file (by path) with an id owns it; later ones are duplicates.
            entry.duplicate = false;
            if (entry.id.IsValid()) {
                const auto [existing, inserted] = m_ById.emplace(entry.id, i);
                entry.duplicate = !inserted;
            }
        }
    }

}  // namespace Frost
