#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
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

    // --benchmark: instead of playing a level, time the physics queries on a scene generated for
    // the run (in memory only: nothing is loaded or saved), print the results and exit.
    struct PhysicsBenchmarkOptions {
        static constexpr int kMaxCount = 1'000'000;  // For --colliders and --queries.
        static constexpr int kMaxBatches = 1'000;

        int colliders = 10'000;  // Boxes and capsules, half each.
        int queries = 100;       // Of each kind, per batch.
        int batches = 3;         // Timed batches (after an untimed warm-up one).
        std::uint32_t seed = 1;  // The same seed gives the same scene and queries.

        friend bool operator==(const PhysicsBenchmarkOptions&, const PhysicsBenchmarkOptions&) = default;
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
    //   FrostGame --benchmark [--colliders <n>] [--queries <n>] [--batches <n>] [--seed <n>]
    struct GameLaunchOptions {
        static constexpr const char* kUsage =
                "Usage: FrostGame [--project <project>] [--level <level>] [--view <x,y,z,yaw,pitch>] [--play-in-editor]\n"
                "       FrostGame --benchmark [--colliders <n>] [--queries <n>] [--batches <n>] [--seed <n>]\n"
                "\n"
                "  --project <project>        The Frost project to run (a directory, or its .frostproject file).\n"
                "                             Default: the working directory, where the build stages the project.\n"
                "  --level <level>            The .flevel file to play. Default: the project's startup level.\n"
                "  --view <x,y,z,yaw,pitch>   Where the free camera starts (degrees), if the level has no camera.\n"
                "  --play-in-editor           Started by the editor: F5 or Escape closes the game.\n"
                "\n"
                "  --benchmark                Time the physics queries (raycasts, shape casts, overlaps) on a scene of\n"
                "                             random colliders generated for the run (never saved), print the average\n"
                "                             time of each kind of query and exit. No window is opened.\n"
                "  --colliders <n>            Colliders in the scene (boxes and capsules, half each). Default: 10000.\n"
                "  --queries <n>              Queries of each kind per batch. Default: 100.\n"
                "  --batches <n>              Timed batches (after one untimed warm-up batch). Default: 3.\n"
                "  --seed <n>                 Seed of the scene and queries (the same seed, the same run). Default: 1.\n";

        std::optional<std::filesystem::path> projectPath;
        std::optional<std::filesystem::path> levelPath;
        std::optional<ViewPoint> startView;
        bool playInEditor = false;
        bool showHelp = false;  // --help / -h.

        // Set by --benchmark (and tuned by --colliders, --queries, --batches, --seed, which need it).
        std::optional<PhysicsBenchmarkOptions> benchmark;

        // The command line (without the program name) that Parse reads back as these options.
        [[nodiscard]]
        std::vector<std::string> ToArguments() const;

        // Reads a command line (without the program name; UTF-8). Throws LaunchOptionsError.
        [[nodiscard]]
        static GameLaunchOptions Parse(std::span<const std::string> arguments);
    };

}  // namespace Frost
