#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Thrown when a process cannot be started.
class ChildProcessError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Another program, started by the editor and running alongside it (the game, for Play In
// Editor). It shares the editor's console, so what it prints shows up with the editor's output.
//
// Destroying a ChildProcess that is still running ends it (forcibly), so a child never outlives
// the object that started it.
class ChildProcess final {
public:
    // Starts `executable` with `arguments` (UTF-8; not including the program name) in
    // `workingDirectory`. Throws ChildProcessError (with the system's reason) if it cannot be
    // started, e.g. the file is missing or not executable.
    ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                 const std::filesystem::path& workingDirectory);

    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    // Never blocks. Nullopt while it is running; once it has exited, its exit code (on POSIX, a
    // process ended by a signal reports 128 + the signal number, as shells do).
    [[nodiscard]]
    std::optional<int> PollExit();

    // Asks it to close, the way closing its window would (Windows: WM_CLOSE to its windows;
    // elsewhere: SIGTERM). It may take a moment; see PollExit.
    void RequestClose();

    // Ends it immediately, and waits until it has.
    void Kill();

    // The process id of this editor (e.g. to name temporary files uniquely).
    [[nodiscard]]
    static unsigned long GetCurrentProcessId() noexcept;

private:
#ifdef _WIN32
    void* m_Process = nullptr;  // HANDLE.
    unsigned long m_Id = 0;
#else
    int m_Pid = -1;
#endif
    std::optional<int> m_ExitCode;
};
