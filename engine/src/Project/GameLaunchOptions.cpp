#include <Frost/Project/GameLaunchOptions.hpp>

#include <Frost/Platform/Filesystem.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace {

    constexpr std::string_view kProject = "--project";
    constexpr std::string_view kLevel = "--level";
    constexpr std::string_view kView = "--view";
    constexpr std::string_view kPlayInEditor = "--play-in-editor";

    // Shortest round-trip form, independent of the C locale (which could otherwise turn "1.5" into
    // "1,5" and break the comma-separated list).
    [[nodiscard]]
    std::string FormatFloat(const float value) {
        std::array<char, 32> buffer{};
        const std::to_chars_result result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        return std::string(buffer.data(), result.ptr);
    }

    [[nodiscard]]
    Frost::ViewPoint ParseView(const std::string_view text) {
        std::array<float, 5> values{};
        std::size_t count = 0;
        const char* cursor = text.data();
        const char* const end = text.data() + text.size();

        while (true) {
            if (count == values.size()) {
                throw Frost::LaunchOptionsError("--view takes exactly five numbers: x,y,z,yaw,pitch.");
            }
            const std::from_chars_result result = std::from_chars(cursor, end, values[count]);
            if (result.ec != std::errc() || !std::isfinite(values[count])) {
                throw Frost::LaunchOptionsError("--view: '" + std::string(text) + "' is not x,y,z,yaw,pitch.");
            }
            ++count;
            cursor = result.ptr;
            if (cursor == end) { break; }
            if (*cursor != ',') {
                throw Frost::LaunchOptionsError("--view: '" + std::string(text) + "' is not x,y,z,yaw,pitch.");
            }
            ++cursor;
        }
        if (count != values.size()) {
            throw Frost::LaunchOptionsError("--view takes exactly five numbers: x,y,z,yaw,pitch.");
        }

        return Frost::ViewPoint{.position = glm::vec3(values[0], values[1], values[2]),
                                .yawDegrees = values[3],
                                .pitchDegrees = values[4]};
    }

}  // namespace

namespace Frost {

    std::vector<std::string> GameLaunchOptions::ToArguments() const {
        std::vector<std::string> arguments;
        if (projectPath.has_value()) {
            arguments.emplace_back(kProject);
            arguments.push_back(PathToUtf8(*projectPath));
        }
        if (levelPath.has_value()) {
            arguments.emplace_back(kLevel);
            arguments.push_back(PathToUtf8(*levelPath));
        }
        if (startView.has_value()) {
            arguments.emplace_back(kView);
            arguments.push_back(FormatFloat(startView->position.x) + "," + FormatFloat(startView->position.y) + "," +
                                FormatFloat(startView->position.z) + "," + FormatFloat(startView->yawDegrees) + "," +
                                FormatFloat(startView->pitchDegrees));
        }
        if (playInEditor) { arguments.emplace_back(kPlayInEditor); }
        if (showHelp) { arguments.emplace_back("--help"); }
        return arguments;
    }

    GameLaunchOptions GameLaunchOptions::Parse(const std::span<const std::string> arguments) {
        GameLaunchOptions options;

        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const std::string_view argument = arguments[i];

            // The value after an option that takes one.
            const auto value = [&]() -> const std::string& {
                if (i + 1 >= arguments.size()) { throw LaunchOptionsError(std::string(argument) + " needs a value."); }
                return arguments[++i];
            };

            if (argument == kProject) { options.projectPath = PathFromUtf8(value()); }
            else if (argument == kLevel) { options.levelPath = PathFromUtf8(value()); }
            else if (argument == kView) { options.startView = ParseView(value()); }
            else if (argument == kPlayInEditor) { options.playInEditor = true; }
            else if (argument == "--help" || argument == "-h") { options.showHelp = true; }
            else { throw LaunchOptionsError("Unexpected argument '" + std::string(argument) + "'."); }
        }

        return options;
    }

}  // namespace Frost
