#include <Frost/Project/GameLaunchOptions.hpp>

#include <Frost/Platform/Filesystem.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

    constexpr std::string_view kProject = "--project";
    constexpr std::string_view kLevel = "--level";
    constexpr std::string_view kView = "--view";
    constexpr std::string_view kPlayInEditor = "--play-in-editor";
    constexpr std::string_view kBenchmark = "--benchmark";
    constexpr std::string_view kColliders = "--colliders";
    constexpr std::string_view kQueries = "--queries";
    constexpr std::string_view kBatches = "--batches";
    constexpr std::string_view kSeed = "--seed";

    // A whole number in [minimum, maximum], all of `text`.
    [[nodiscard]]
    long long ParseCount(const std::string_view option, const std::string_view text, const long long minimum,
                         const long long maximum) {
        long long value = 0;
        const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size() || value < minimum || value > maximum) {
            throw Frost::LaunchOptionsError(std::string(option) + " takes a whole number from " + std::to_string(minimum) +
                                            " to " + std::to_string(maximum) + ", not '" + std::string(text) + "'.");
        }
        return value;
    }

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
        if (benchmark.has_value()) {
            arguments.emplace_back(kBenchmark);
            for (const auto& [option, value] : {std::pair{kColliders, static_cast<long long>(benchmark->colliders)},
                                                std::pair{kQueries, static_cast<long long>(benchmark->queries)},
                                                std::pair{kBatches, static_cast<long long>(benchmark->batches)},
                                                std::pair{kSeed, static_cast<long long>(benchmark->seed)}}) {
                arguments.emplace_back(option);
                arguments.push_back(std::to_string(value));
            }
        }
        if (showHelp) { arguments.emplace_back("--help"); }
        return arguments;
    }

    GameLaunchOptions GameLaunchOptions::Parse(const std::span<const std::string> arguments) {
        GameLaunchOptions options;
        bool benchmark = false;
        PhysicsBenchmarkOptions benchmarkOptions;
        std::optional<std::string_view> benchmarkSetting;  // The first --colliders etc. given, if any.

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
            else if (argument == kBenchmark) { benchmark = true; }
            else if (argument == kColliders || argument == kQueries || argument == kBatches || argument == kSeed) {
                if (!benchmarkSetting.has_value()) { benchmarkSetting = argument; }
                const std::string& text = value();
                if (argument == kColliders) {
                    benchmarkOptions.colliders = static_cast<int>(ParseCount(argument, text, 0, PhysicsBenchmarkOptions::kMaxCount));
                }
                else if (argument == kQueries) {
                    benchmarkOptions.queries = static_cast<int>(ParseCount(argument, text, 1, PhysicsBenchmarkOptions::kMaxCount));
                }
                else if (argument == kBatches) {
                    benchmarkOptions.batches = static_cast<int>(ParseCount(argument, text, 1, PhysicsBenchmarkOptions::kMaxBatches));
                }
                else { benchmarkOptions.seed = static_cast<std::uint32_t>(ParseCount(argument, text, 0, 4294967295LL)); }
            }
            else if (argument == "--help" || argument == "-h") { options.showHelp = true; }
            else { throw LaunchOptionsError("Unexpected argument '" + std::string(argument) + "'."); }
        }

        if (benchmarkSetting.has_value() && !benchmark) {
            throw LaunchOptionsError(std::string(*benchmarkSetting) + " is a benchmark setting: it needs --benchmark.");
        }
        if (benchmark) {
            // The benchmark runs on its own generated scene, never on a level (nor is one written).
            if (options.levelPath.has_value() || options.startView.has_value() || options.playInEditor) {
                throw LaunchOptionsError("--benchmark runs on a scene of its own: it takes no --level, --view or "
                                         "--play-in-editor.");
            }
            options.benchmark = benchmarkOptions;
        }

        return options;
    }

}  // namespace Frost
