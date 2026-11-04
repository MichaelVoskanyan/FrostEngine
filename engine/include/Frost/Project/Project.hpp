#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace Frost {

    // Thrown by Project::Open for a path that is not a Frost project.
    class ProjectError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // A Frost project: one self-contained directory holding everything a game needs, which the
    // editor and the game each work on one of at a time:
    //
    //   <root>/
    //     <Name>.frostproject   the project descriptor (optional for now; see below)
    //     content/              the project's Frost assets (.fmesh, .ftex), in any folder structure
    //       Levels/             its levels (.flevel); where new levels are saved by default
    //     assets/               source files (glTF, PNG, ...) by convention; imported into content/
    //
    // A project only ever uses its own content: asset references resolve inside its content
    // directory and nowhere else (see AssetRegistry), and levels live in it. Nothing is read from,
    // or imported automatically out of, another project. Assets move between projects as files:
    // copy a model's folder (its .fmesh and the .ftex files it uses) into another project's content
    // directory and it works there, as references are by asset id.
    //
    // The .frostproject descriptor is where project settings (display name, startup level, engine
    // version, ...) will live. Its contents are not read yet: for now it only marks a directory as
    // a project and names it, so the editor can already be pointed at one.
    class Project final {
    public:
        static constexpr const char* kDescriptorExtension = ".frostproject";
        static constexpr const char* kContentDirectoryName = "content";
        static constexpr const char* kLevelsDirectoryName = "Levels";
        static constexpr const char* kSourceDirectoryName = "assets";
        static constexpr const char* kLevelExtension = ".flevel";

        // The project rooted at `rootDirectory` (made absolute), whether or not it exists yet: no
        // checks (see Open). A packaged game runs as the project in its own directory.
        explicit Project(const std::filesystem::path& rootDirectory);

        // The project at `path`: a project directory, or a .frostproject file (its directory is
        // the project). A directory is a project if it has a .frostproject file or a content
        // directory. Throws ProjectError otherwise, or if a directory holds several descriptors.
        [[nodiscard]]
        static Project Open(const std::filesystem::path& path);

        // Whether `directory` looks like a project (see Open).
        [[nodiscard]]
        static bool IsProjectDirectory(const std::filesystem::path& directory);

        // The descriptor's stem if there is one, else the root directory's name.
        [[nodiscard]]
        const std::string& GetName() const noexcept;

        [[nodiscard]]
        const std::filesystem::path& GetRootDirectory() const noexcept;

        // The .frostproject file, if the project has one.
        [[nodiscard]]
        const std::optional<std::filesystem::path>& GetDescriptorPath() const noexcept;

        // <root>/content: every asset and level of the project.
        [[nodiscard]]
        std::filesystem::path GetContentDirectory() const;

        // <root>/content/Levels: the default place for levels.
        [[nodiscard]]
        std::filesystem::path GetLevelsDirectory() const;

        // <root>/assets: where source files are kept by convention (may not exist).
        [[nodiscard]]
        std::filesystem::path GetSourceDirectory() const;

        // Whether `path` is the content directory or inside it: part of what the project uses.
        [[nodiscard]]
        bool IsInContent(const std::filesystem::path& path) const;

        // Whether `path` is the root directory or inside it.
        [[nodiscard]]
        bool IsInProject(const std::filesystem::path& path) const;

        // Creates the content and levels directories if they are missing. Throws ProjectError.
        void CreateDirectories() const;

    private:
        std::filesystem::path m_Root;
        std::optional<std::filesystem::path> m_Descriptor;
        std::string m_Name;
    };

}  // namespace Frost
