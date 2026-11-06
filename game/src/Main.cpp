#include "GameApplication.hpp"

#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Project/GameLaunchOptions.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
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
    #include <shellapi.h>
#endif

namespace {

    // The command line (without the program name) as UTF-8. On Windows, argv is in the ANSI code
    // page, which cannot hold every path (the editor passes paths of the project and of a
    // temporary level); the UTF-16 command line can.
    [[nodiscard]]
    std::vector<std::string> GetArguments([[maybe_unused]] const int argc, [[maybe_unused]] char** const argv) {
        std::vector<std::string> arguments;
#ifdef _WIN32
        int count = 0;
        LPWSTR* const wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (wide != nullptr) {
            for (int i = 1; i < count; ++i) { arguments.push_back(Frost::PathToUtf8(std::filesystem::path(wide[i]))); }
            LocalFree(wide);
            return arguments;
        }
#endif
        for (int i = 1; i < argc; ++i) { arguments.emplace_back(argv[i]); }
        return arguments;
    }

}  // namespace

int main(const int argc, char** const argv) {
    Frost::GameLaunchOptions options;
    try {
        options = Frost::GameLaunchOptions::Parse(GetArguments(argc, argv));
    }
    catch (const Frost::LaunchOptionsError& e) {
        std::cerr << e.what() << "\n\n" << Frost::GameLaunchOptions::kUsage;
        return EXIT_FAILURE;
    }

    if (options.showHelp) {
        std::cout << Frost::GameLaunchOptions::kUsage;
        return EXIT_SUCCESS;
    }

    try {
        GameApplication application(options);
        application.Run();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}