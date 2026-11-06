#include "ChildProcess.hpp"

#include <Frost/Platform/Filesystem.hpp>

#include <string>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <cstring>
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#ifdef _WIN32

namespace {

    // One argument as the C runtime's command-line parser (CommandLineToArgvW) reads it back:
    // quoted if it has spaces or quotes, with backslashes doubled only where they precede a quote.
    [[nodiscard]]
    std::wstring QuoteArgument(const std::wstring& argument) {
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) { return argument; }

        std::wstring quoted = L"\"";
        for (auto it = argument.begin();; ++it) {
            std::size_t backslashes = 0;
            while (it != argument.end() && *it == L'\\') {
                ++it;
                ++backslashes;
            }

            if (it == argument.end()) {
                // Doubled: they precede the closing quote.
                quoted.append(backslashes * 2, L'\\');
                break;
            }
            if (*it == L'"') {
                // Doubled, plus one to escape the quote itself.
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'"');
            }
            else {
                quoted.append(backslashes, L'\\');
                quoted.push_back(*it);
            }
        }
        quoted.push_back(L'"');
        return quoted;
    }

    [[nodiscard]]
    std::string DescribeLastError() {
        const DWORD error = GetLastError();
        LPWSTR buffer = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                            nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        std::string message = length > 0 ? Frost::PathToUtf8(std::filesystem::path(std::wstring(buffer, length)))
                                         : "error " + std::to_string(error);
        if (buffer != nullptr) { LocalFree(buffer); }
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
            message.pop_back();
        }
        return message;
    }

    // Posts WM_CLOSE to every top-level window of the process `lParam` points at.
    BOOL CALLBACK CloseWindowsOfProcess(HWND window, LPARAM lParam) {
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == *reinterpret_cast<const DWORD*>(lParam)) { PostMessageW(window, WM_CLOSE, 0, 0); }
        return TRUE;  // Keep enumerating.
    }

}  // namespace

ChildProcess::ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                           const std::filesystem::path& workingDirectory) {
    std::wstring commandLine = QuoteArgument(executable.wstring());
    for (const std::string& argument : arguments) {
        commandLine += L' ';
        commandLine += QuoteArgument(Frost::PathFromUtf8(argument).wstring());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};

    // No handles inherited; without CREATE_NEW_CONSOLE a console program shares the editor's.
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workingDirectory.c_str(), &startup, &info)) {
        throw ChildProcessError("Could not start '" + Frost::PathToUtf8(executable) + "': " + DescribeLastError() + ".");
    }

    CloseHandle(info.hThread);
    m_Process = info.hProcess;
    m_Id = info.dwProcessId;
}

ChildProcess::~ChildProcess() {
    if (!PollExit().has_value()) { Kill(); }
    if (m_Process != nullptr) { CloseHandle(static_cast<HANDLE>(m_Process)); }
}

std::optional<int> ChildProcess::PollExit() {
    if (m_ExitCode.has_value()) { return m_ExitCode; }

    if (WaitForSingleObject(static_cast<HANDLE>(m_Process), 0) != WAIT_OBJECT_0) { return std::nullopt; }

    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(m_Process), &code);
    m_ExitCode = static_cast<int>(code);
    return m_ExitCode;
}

void ChildProcess::RequestClose() {
    if (PollExit().has_value()) { return; }

    DWORD id = m_Id;
    EnumWindows(CloseWindowsOfProcess, reinterpret_cast<LPARAM>(&id));
}

void ChildProcess::Kill() {
    if (PollExit().has_value()) { return; }

    TerminateProcess(static_cast<HANDLE>(m_Process), 1);
    WaitForSingleObject(static_cast<HANDLE>(m_Process), INFINITE);
    static_cast<void>(PollExit());
}

unsigned long ChildProcess::GetCurrentProcessId() noexcept { return ::GetCurrentProcessId(); }

#else  // POSIX

ChildProcess::ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                           const std::filesystem::path& workingDirectory) {
    // Everything the child needs is prepared before fork(): between fork() and exec() a
    // multithreaded program may only make async-signal-safe calls (no allocation).
    const std::string program = executable.string();
    const std::string directory = workingDirectory.string();

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(program);
    storage.insert(storage.end(), arguments.begin(), arguments.end());

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& argument : storage) { argv.push_back(argument.data()); }
    argv.push_back(nullptr);

    // exec() failing happens in the child, after fork() has already succeeded. The child reports
    // it through this pipe; on success exec() closes the pipe (close-on-exec) without a word, so
    // the parent reading end-of-file means the program is running.
    int report[2] = {-1, -1};
    if (pipe(report) != 0) {
        throw ChildProcessError("Could not start '" + program + "': " + std::strerror(errno) + ".");
    }
    fcntl(report[0], F_SETFD, FD_CLOEXEC);
    fcntl(report[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid = fork();
    if (pid < 0) {
        const int error = errno;
        close(report[0]);
        close(report[1]);
        throw ChildProcessError("Could not start '" + program + "': " + std::strerror(error) + ".");
    }

    if (pid == 0) {
        // The child. Only async-signal-safe calls from here on.
        close(report[0]);
        int error = 0;
        if (chdir(directory.c_str()) != 0) { error = errno; }
        else {
            execv(program.c_str(), argv.data());
            error = errno;  // Only reached if exec failed.
        }
        ssize_t written = 0;
        do { written = write(report[1], &error, sizeof(error)); } while (written < 0 && errno == EINTR);
        _exit(127);
    }

    close(report[1]);
    int childError = 0;
    ssize_t received = 0;
    do { received = read(report[0], &childError, sizeof(childError)); } while (received < 0 && errno == EINTR);
    close(report[0]);

    if (received == static_cast<ssize_t>(sizeof(childError))) {
        int status = 0;
        waitpid(pid, &status, 0);  // Reap it: it has exited already (or is about to).
        throw ChildProcessError("Could not start '" + program + "': " + std::strerror(childError) + ".");
    }

    m_Pid = pid;
}

ChildProcess::~ChildProcess() {
    if (!PollExit().has_value()) { Kill(); }
}

std::optional<int> ChildProcess::PollExit() {
    if (m_ExitCode.has_value()) { return m_ExitCode; }

    int status = 0;
    const pid_t result = waitpid(m_Pid, &status, WNOHANG);
    if (result == 0) { return std::nullopt; }  // Still running.

    if (result == m_Pid) {
        m_ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    }
    else {
        m_ExitCode = -1;  // Cannot be waited for (should not happen): treat it as gone.
    }
    return m_ExitCode;
}

void ChildProcess::RequestClose() {
    if (!PollExit().has_value()) { kill(m_Pid, SIGTERM); }
}

void ChildProcess::Kill() {
    if (PollExit().has_value()) { return; }

    kill(m_Pid, SIGKILL);
    int status = 0;
    pid_t result = 0;
    do { result = waitpid(m_Pid, &status, 0); } while (result < 0 && errno == EINTR);
    m_ExitCode = result == m_Pid && WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

unsigned long ChildProcess::GetCurrentProcessId() noexcept { return static_cast<unsigned long>(getpid()); }

#endif
