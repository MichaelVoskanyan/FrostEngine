#pragma once

#include <glm/vec3.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Frost {

    // A place to look from: a position, and a direction as yaw (degrees about world +Y; positive
    // turns left, 0 looks down -Z) and pitch (degrees; positive looks up). No roll.
    struct ViewPoint {
        glm::vec3 position{0.0f};
        float yawDegrees = 0.0f;
        float pitchDegrees = 0.0f;
    };

    // Thrown by GameLaunchOptions::Parse for a command line it does not understand.
    class LaunchOptionsError final : public std::invalid_argument {
    public:
        using std::invalid_argument::invalid_argument;
    };

    // How the game is started: its command line. The editor uses this to start the game on the level
    // being edited (Play In Editor) and the game to read it, so both always agree on the format.
    //
    //   FrostGame [--project <dir>] [--level <file>] [--view <x,y,z,yaw,pitch>] [--play-in-editor]
    struct GameLaunchOptions {
        static constexpr const char* kUsage =
                "Usage: FrostGame [--project <project>] [--level <level>] [--view <x,y,z,yaw,pitch>] [--play-in-editor]\n"
                "\n"
                "  --project <project>        The Frost project to run (a directory, or its .frostproject file).\n"
                "                             Default: the working directory, where the build stages the project.\n"
                "  --level <level>            The .flevel file to play. Default: the project's startup level.\n"
                "  --view <x,y,z,yaw,pitch>   Where the free camera starts (degrees), if the level has no camera.\n"
                "  --play-in-editor           Started by the editor: F5 or Escape closes the game.\n";

        std::optional<std::filesystem::path> projectPath;
        std::optional<std::filesystem::path> levelPath;
        std::optional<ViewPoint> startView;
        bool playInEditor = false;
        bool showHelp = false;  // --help / -h.

        // The command line (without the program name) that Parse reads back as these options.
        [[nodiscard]]
        std::vector<std::string> ToArguments() const;

        // Reads a command line (without the program name; UTF-8). Throws LaunchOptionsError.
        [[nodiscard]]
        static GameLaunchOptions Parse(std::span<const std::string> arguments);
    };

}  // namespace Frost
