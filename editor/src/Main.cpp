#include "EditorApplication.hpp"

#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Project/Project.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>

namespace {

    constexpr const char* kUsage =
            "Usage: FrostEditor [<project>]\n"
            "       FrostEditor --project <project>\n"
            "\n"
            "<project> is a Frost project directory (one with a 'content' directory or a .frostproject\n"
            "file), or a .frostproject file. Without one, the editor opens the project it was built for.\n";

    // The project to open: the one named on the command line, else the project this editor was
    // built for (if it still exists), else the working directory if that is a project. Nullopt
    // (after printing why) if there is none, or the arguments are not understood.
    [[nodiscard]]
    std::optional<Frost::Project> ResolveProject(const int argc, char** const argv) {
        std::optional<std::filesystem::path> requested;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--project" && i + 1 < argc) { requested = Frost::PathFromUtf8(argv[++i]); }
            else if (!argument.starts_with("-") && !requested.has_value()) { requested = Frost::PathFromUtf8(argument); }
            else {
                std::cerr << "Unexpected argument '" << argument << "'.\n\n" << kUsage;
                return std::nullopt;
            }
        }

        try {
            if (requested.has_value()) { return Frost::Project::Open(*requested); }

#ifdef FROST_EDITOR_DEFAULT_PROJECT_DIR
            const std::filesystem::path builtFor = Frost::PathFromUtf8(FROST_EDITOR_DEFAULT_PROJECT_DIR);
            if (Frost::Project::IsProjectDirectory(builtFor)) { return Frost::Project::Open(builtFor); }
            std::cerr << "[Editor] The project this editor was built for ('" << FROST_EDITOR_DEFAULT_PROJECT_DIR
                      << "') is not there any more; trying the working directory.\n";
#endif
            std::error_code error;
            return Frost::Project::Open(std::filesystem::current_path(error));
        }
        catch (const Frost::ProjectError& e) {
            std::cerr << "Cannot open the project: " << e.what() << "\n\n" << kUsage;
            return std::nullopt;
        }
    }

}  // namespace

int main(const int argc, char** const argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
            std::cout << kUsage;
            return EXIT_SUCCESS;
        }
    }

    const std::optional<Frost::Project> project = ResolveProject(argc, argv);
    if (!project.has_value()) { return EXIT_FAILURE; }

    try {
        EditorApplication application(*project);
        application.Run();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
