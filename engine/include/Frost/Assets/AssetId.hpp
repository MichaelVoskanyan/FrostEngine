#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Frost {

    // An asset's permanent identity: a random 128-bit value assigned when the asset is first
    // created (imported), stored in its file's header, and kept for the asset's whole life - through
    // reimports, renames and moves, whether made in the editor or in the OS file manager. Everything
    // that refers to an asset (levels, a mesh's materials referring to their textures) stores this,
    // so references survive the file being reorganized. The all-zero value means "none".
    struct AssetId {
        std::uint64_t high = 0;
        std::uint64_t low = 0;

        // A new random id (version-4 UUID layout).
        [[nodiscard]]
        static AssetId Generate();

        // 32 lowercase hex digits, or nullopt if `text` is not that.
        [[nodiscard]]
        static std::optional<AssetId> Parse(std::string_view text);

        [[nodiscard]]
        std::string ToString() const;

        [[nodiscard]]
        bool IsValid() const noexcept {
            return high != 0 || low != 0;
        }

        auto operator<=>(const AssetId&) const = default;
    };

    // How one asset refers to another: by id, plus the path the target was last known at.
    //
    // The id is what counts: AssetRegistry finds the target wherever it is now. The path is a
    // fallback for targets the registry cannot see (outside the content directory, or assets from
    // before ids existed), and keeps the reference readable in files. Both may be empty (no
    // reference).
    struct AssetReference {
        AssetId id;
        std::filesystem::path path;  // Normalized absolute, when set.

        [[nodiscard]]
        bool IsSet() const noexcept {
            return id.IsValid() || !path.empty();
        }
    };

}  // namespace Frost

template <>
struct std::hash<Frost::AssetId> {
    std::size_t operator()(const Frost::AssetId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.high ^ (id.low * 0x9E3779B97F4A7C15ull));
    }
};
