#include <Frost/Platform/KeyNames.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace Frost {

    namespace {

        using Entry = std::pair<Key, std::string_view>;

        constexpr std::array kKeys{
                Entry{Key::A, "A"},
                Entry{Key::B, "B"},
                Entry{Key::C, "C"},
                Entry{Key::D, "D"},
                Entry{Key::E, "E"},
                Entry{Key::F, "F"},
                Entry{Key::G, "G"},
                Entry{Key::H, "H"},
                Entry{Key::I, "I"},
                Entry{Key::J, "J"},
                Entry{Key::K, "K"},
                Entry{Key::L, "L"},
                Entry{Key::M, "M"},
                Entry{Key::N, "N"},
                Entry{Key::O, "O"},
                Entry{Key::P, "P"},
                Entry{Key::Q, "Q"},
                Entry{Key::R, "R"},
                Entry{Key::S, "S"},
                Entry{Key::T, "T"},
                Entry{Key::U, "U"},
                Entry{Key::V, "V"},
                Entry{Key::W, "W"},
                Entry{Key::X, "X"},
                Entry{Key::Y, "Y"},
                Entry{Key::Z, "Z"},
                Entry{Key::D0, "0"},
                Entry{Key::D1, "1"},
                Entry{Key::D2, "2"},
                Entry{Key::D3, "3"},
                Entry{Key::D4, "4"},
                Entry{Key::D5, "5"},
                Entry{Key::D6, "6"},
                Entry{Key::D7, "7"},
                Entry{Key::D8, "8"},
                Entry{Key::D9, "9"},
                Entry{Key::F1, "F1"},
                Entry{Key::F2, "F2"},
                Entry{Key::F3, "F3"},
                Entry{Key::F4, "F4"},
                Entry{Key::F5, "F5"},
                Entry{Key::F6, "F6"},
                Entry{Key::F7, "F7"},
                Entry{Key::F8, "F8"},
                Entry{Key::F9, "F9"},
                Entry{Key::F10, "F10"},
                Entry{Key::F11, "F11"},
                Entry{Key::F12, "F12"},
                Entry{Key::Space, "Space"},
                Entry{Key::Enter, "Enter"},
                Entry{Key::Tab, "Tab"},
                Entry{Key::Backspace, "Backspace"},
                Entry{Key::Escape, "Escape"},
                Entry{Key::Insert, "Insert"},
                Entry{Key::Delete, "Delete"},
                Entry{Key::Home, "Home"},
                Entry{Key::End, "End"},
                Entry{Key::PageUp, "Page Up"},
                Entry{Key::PageDown, "Page Down"},
                Entry{Key::Up, "Up"},
                Entry{Key::Down, "Down"},
                Entry{Key::Left, "Left"},
                Entry{Key::Right, "Right"},
                Entry{Key::LeftShift, "Left Shift"},
                Entry{Key::RightShift, "Right Shift"},
                Entry{Key::LeftControl, "Left Control"},
                Entry{Key::RightControl, "Right Control"},
                Entry{Key::LeftAlt, "Left Alt"},
                Entry{Key::RightAlt, "Right Alt"},
                Entry{Key::LeftSuper, "Left Super"},
                Entry{Key::RightSuper, "Right Super"},
        };

        constexpr std::array<Key, kKeys.size()> kKeyList = [] {
            std::array<Key, kKeys.size()> keys{};
            for (std::size_t i = 0; i < kKeys.size(); ++i) { keys[i] = kKeys[i].first; }
            return keys;
        }();

        [[nodiscard]]
        bool EqualsIgnoringCase(const std::string_view a, const std::string_view b) noexcept {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        }

    }  // namespace

    std::span<const Key> GetNamedKeys() noexcept { return kKeyList; }

    std::string_view GetKeyName(const Key key) noexcept {
        const auto entry = std::find_if(kKeys.begin(), kKeys.end(), [key](const Entry& e) { return e.first == key; });
        return entry != kKeys.end() ? entry->second : std::string_view{};
    }

    std::optional<Key> FindKey(const std::string_view name) noexcept {
        const auto entry = std::find_if(kKeys.begin(), kKeys.end(), [name](const Entry& e) { return EqualsIgnoringCase(e.second, name); });
        if (entry == kKeys.end()) { return std::nullopt; }
        return entry->first;
    }

}  // namespace Frost
