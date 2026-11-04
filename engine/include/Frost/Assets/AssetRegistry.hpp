#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Assets/AssetType.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Frost {

    // Where every asset in a content directory is: a map from asset id to the file currently
    // holding it, built by scanning the directory tree and reading each asset file's header.
    //
    // This is what makes references survive reorganizing: a level or mesh refers to an asset by
    // id, and the registry finds that id wherever the file now is - renamed or moved in the
    // editor, or in the OS file manager while the editor was closed (or open: Refresh() again).
    // Refresh() is incremental: only files that are new or whose size/modification time changed
    // have their header read.
    //
    // Two files with the same id (typically a copy made outside the editor) are both listed, but
    // lookups by id resolve to the first by path; the other is flagged (Entry::duplicate) so a
    // tool can give it an identity of its own.
    class AssetRegistry final {
    public:
        struct Entry {
            AssetId id;  // Invalid for version-1 files, which predate ids.
            AssetType type = AssetType::Mesh;
            std::uint32_t version = 0;
            std::filesystem::path path;  // Normalized absolute.
            std::filesystem::path sourcePath;
            std::string sourceItem;
            std::uintmax_t size = 0;
            std::filesystem::file_time_type modified{};
            bool duplicate = false;  // Another file (earlier by path) has the same id.
        };

        AssetRegistry() = default;

        // Sets the content directory and scans it.
        explicit AssetRegistry(std::filesystem::path root);

        void SetRoot(std::filesystem::path root);

        [[nodiscard]]
        const std::filesystem::path& GetRoot() const noexcept;

        // The project directory: the content directory's parent. Paths that asset files carry
        // (their source files, ...) are stored relative to it when inside it (see AssetFile).
        [[nodiscard]]
        std::filesystem::path GetProjectDirectory() const;

        // Rescans the root. Returns whether anything changed (assets added, removed, moved or
        // modified) since the previous scan.
        bool Refresh();

        // Every asset, sorted by path.
        [[nodiscard]]
        const std::vector<Entry>& GetEntries() const noexcept;

        // Incremented by every Refresh() that found a change; lets callers cache derived data.
        [[nodiscard]]
        std::uint64_t GetGeneration() const noexcept;

        [[nodiscard]]
        const Entry* Find(const AssetId& id) const;

        [[nodiscard]]
        const Entry* FindByPath(const std::filesystem::path& path) const;

        // The asset imported from `source` (and, for assets made from part of a source file, that
        // item of it), if any.
        [[nodiscard]]
        const Entry* FindBySource(const std::filesystem::path& source, std::string_view item, AssetType type) const;

        // The file a reference points at now: found by id if the registry knows it, else the
        // reference's path if that file is inside the content directory, exists, and is a Frost
        // asset whose id matches (or the reference has none). Nullopt if the target cannot be
        // found. Nothing outside the content directory is ever resolved (a project only uses its
        // own assets), unless the registry has no root, in which case paths are taken as they are.
        [[nodiscard]]
        std::optional<std::filesystem::path> Resolve(const AssetReference& reference) const;

        // Whether `path` is inside the content directory.
        [[nodiscard]]
        bool Contains(const std::filesystem::path& path) const;

    private:
        void Rebuild();

        std::filesystem::path m_Root;
        std::vector<Entry> m_Entries;
        std::unordered_map<AssetId, std::size_t> m_ById;  // Index into m_Entries (first by path).
        std::map<std::filesystem::path, std::size_t> m_ByPath;
        std::uint64_t m_Generation = 0;
    };

}  // namespace Frost
