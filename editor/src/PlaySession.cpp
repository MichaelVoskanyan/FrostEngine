#include "PlaySession.hpp"

#include <Frost/Assets/AssetRegistry.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Project/Project.hpp>
#include <Frost/Scene/Level.hpp>
#include <Frost/Scene/LevelSerializer.hpp>

#include <string>
#include <system_error>
#include <utility>

namespace {

    // How long a game asked to close gets before it is ended forcibly.
    constexpr std::chrono::seconds kCloseTimeout{3};

}  // namespace

std::filesystem::path PlaySession::GetDefaultGameExecutable() {
#ifdef FROST_GAME_EXECUTABLE
    return Frost::PathFromUtf8(FROST_GAME_EXECUTABLE);
#else
    return {};
#endif
}

PlaySession::PlaySession(std::filesystem::path gameExecutable) : m_GameExecutable(std::move(gameExecutable)) {}

PlaySession::~PlaySession() {
    if (m_Process != nullptr) {
        m_Process->Kill();
        Finish();
    }
}

std::string PlaySession::Start(const Frost::Level& level, const Frost::Project& project,
                               const Frost::AssetRegistry& registry, const Frost::ViewPoint& view) {
    if (m_Process != nullptr) { return {}; }

    std::error_code error;
    if (m_GameExecutable.empty() || !std::filesystem::is_regular_file(m_GameExecutable, error)) {
        return "The game executable was not found" +
               (m_GameExecutable.empty() ? std::string(".") : " at '" + Frost::PathToUtf8(m_GameExecutable) + "'.") +
               " Build the FrostGame target (building FrostEditor builds it too).";
    }

    // The snapshot is not part of the project (it must not show up in the content directory, or
    // be mistaken for a real level); assets are found by id, so the game resolves them from the
    // project wherever the file is.
    const std::filesystem::path snapshot = GetSnapshotPath();
    std::filesystem::create_directories(snapshot.parent_path(), error);
    try {
        Frost::LevelSerializer::Save(level, snapshot, &registry);
    }
    catch (const Frost::LevelError& e) {
        return std::string("Could not save the level for the game: ") + e.what();
    }

    Frost::GameLaunchOptions options;
    options.projectPath = project.GetRootDirectory();
    options.levelPath = snapshot;
    options.startView = view;
    options.playInEditor = true;

    try {
        // The game finds its shaders relative to its working directory: its own.
        m_Process = std::make_unique<ChildProcess>(m_GameExecutable, options.ToArguments(), m_GameExecutable.parent_path());
    }
    catch (const ChildProcessError& e) {
        std::filesystem::remove(snapshot, error);
        return e.what();
    }

    m_SnapshotPath = snapshot;
    m_StopRequested.reset();
    return {};
}

void PlaySession::Stop() {
    if (m_Process == nullptr) { return; }

    if (m_StopRequested.has_value()) {
        m_Process->Kill();  // Asked twice: no more waiting.
        return;
    }

    m_StopRequested = std::chrono::steady_clock::now();
    m_Process->RequestClose();
}

std::optional<std::string> PlaySession::Update() {
    if (m_Process == nullptr) { return std::nullopt; }

    std::optional<int> exitCode = m_Process->PollExit();
    if (!exitCode.has_value()) {
        if (m_StopRequested.has_value() && std::chrono::steady_clock::now() - *m_StopRequested > kCloseTimeout) {
            m_Process->Kill();
            exitCode = m_Process->PollExit();
        }
        if (!exitCode.has_value()) { return std::nullopt; }
    }

    const bool stopped = m_StopRequested.has_value();
    Finish();

    if (stopped || *exitCode == 0) { return std::nullopt; }
    return "The game stopped with exit code " + std::to_string(*exitCode) +
           " (its console output says why, e.g. a level or asset it could not load).";
}

bool PlaySession::IsRunning() const noexcept { return m_Process != nullptr; }

const std::filesystem::path& PlaySession::GetGameExecutable() const noexcept { return m_GameExecutable; }

std::filesystem::path PlaySession::GetSnapshotPath() {
    std::error_code error;
    std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error) { directory = std::filesystem::current_path(error); }

    // Per editor process, so two editors playing at once do not overwrite each other's level.
    return directory / "FrostEditor" /
           ("PlayInEditor-" + std::to_string(ChildProcess::GetCurrentProcessId()) + Frost::Project::kLevelExtension);
}

void PlaySession::Finish() {
    m_Process.reset();
    m_StopRequested.reset();

    std::error_code error;
    if (!m_SnapshotPath.empty()) { std::filesystem::remove(m_SnapshotPath, error); }
    m_SnapshotPath.clear();
}
