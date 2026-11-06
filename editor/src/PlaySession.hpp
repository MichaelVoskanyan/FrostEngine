#pragma once

#include "Platform/ChildProcess.hpp"

#include <Frost/Project/GameLaunchOptions.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Frost {
    class AssetRegistry;
    class Level;
    class Project;
}  // namespace Frost

// Play In Editor: the level being edited, played in the game (FrostGame) in a window of its own,
// much like Unreal's "Play in Standalone Game". The game gets a snapshot of the level as it is at
// the moment play starts, unsaved changes included, saved to a temporary file; it runs on the
// editor's project directly (not the copy staged next to the game), so assets imported a moment
// ago are there too. What happens in the game never comes back into the editor.
class PlaySession final {
public:
    // The game executable this editor was built with (FROST_GAME_EXECUTABLE, set by CMake).
    [[nodiscard]]
    static std::filesystem::path GetDefaultGameExecutable();

    explicit PlaySession(std::filesystem::path gameExecutable = GetDefaultGameExecutable());

    // Ends a game that is still running.
    ~PlaySession();

    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    PlaySession(PlaySession&&) = delete;
    PlaySession& operator=(PlaySession&&) = delete;

    // Starts the game on a snapshot of `level`, in `project`, with its free camera (if the level has
    // no camera) at `view`. Returns an error message (empty on success). Does nothing if a game is
    // already running.
    [[nodiscard]]
    std::string Start(const Frost::Level& level, const Frost::Project& project, const Frost::AssetRegistry& registry,
                      const Frost::ViewPoint& view);

    // Asks the game to close. If it has not after a few seconds, Update ends it forcibly; asking
    // again while it is closing ends it at once.
    void Stop();

    // Call once per frame: notices the game closing (by itself, e.g. its window was closed, or
    // because it was asked to) and cleans up. Returns a message if it ended with an error (a
    // non-zero exit code that was not the result of Stop).
    [[nodiscard]]
    std::optional<std::string> Update();

    // True from Start until the game has exited (including while it is closing).
    [[nodiscard]]
    bool IsRunning() const noexcept;

    [[nodiscard]]
    const std::filesystem::path& GetGameExecutable() const noexcept;

    // Where the level snapshot for this editor process goes.
    [[nodiscard]]
    static std::filesystem::path GetSnapshotPath();

private:
    void Finish();

    std::filesystem::path m_GameExecutable;
    std::unique_ptr<ChildProcess> m_Process;
    std::filesystem::path m_SnapshotPath;  // The level file the running game was given.
    std::optional<std::chrono::steady_clock::time_point> m_StopRequested;
};
