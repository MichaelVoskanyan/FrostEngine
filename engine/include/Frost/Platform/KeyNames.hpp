#pragma once

#include <Frost/Platform/Input.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace Frost {

    // Names for the keyboard keys (Key), as shown to users and saved in levels: "R", "5", "Space",
    // "F1", "Left Shift", ...

    // Every key that has a name, in a sensible order for a list to pick from (letters, digits,
    // function keys, then the rest).
    [[nodiscard]]
    std::span<const Key> GetNamedKeys() noexcept;

    // The key's name; empty for a value that is not one of the keys.
    [[nodiscard]]
    std::string_view GetKeyName(Key key) noexcept;

    // The key with that name (ignoring case), or nullopt if there is none.
    [[nodiscard]]
    std::optional<Key> FindKey(std::string_view name) noexcept;

}  // namespace Frost
