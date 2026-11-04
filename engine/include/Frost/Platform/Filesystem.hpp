#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Frost {

    // Conversions between std::filesystem::path and UTF-8 text (what JSON files and ImGui use).
    // path::string() is not that: on Windows it converts through the active ANSI code page, which
    // mangles (or throws on) characters outside it. These go through char8_t explicitly.

    [[nodiscard]]
    inline std::string PathToUtf8(const std::filesystem::path& path) {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    // With '/' separators on every platform (for files meant to be shared across platforms).
    [[nodiscard]]
    inline std::string GenericPathToUtf8(const std::filesystem::path& path) {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    [[nodiscard]]
    inline std::filesystem::path PathFromUtf8(const std::string_view text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // How files that refer to other files (levels -> assets, assets -> their sources) store the
    // reference: relative to the referring file's directory when the target is near it (on the
    // same root, at most `maxParentSteps` ".." up, e.g. a sibling folder), so a project folder can
    // be moved or shared; absolute otherwise (something elsewhere on disk, or on another drive),
    // which survives the referring file being moved. Both paths should be absolute and normalized.
    inline constexpr int kMaxPortableParentSteps = 2;

    [[nodiscard]]
    inline std::filesystem::path MakePortablePath(const std::filesystem::path& target,
                                                  const std::filesystem::path& baseDirectory,
                                                  const int maxParentSteps = kMaxPortableParentSteps) {
        if (target.empty() || target.root_name() != baseDirectory.root_name()) { return target; }

        const std::filesystem::path relative = target.lexically_relative(baseDirectory);
        if (relative.empty()) { return target; }

        int parentSteps = 0;
        for (const std::filesystem::path& part : relative) {
            if (part != "..") { break; }
            ++parentSteps;
        }
        return parentSteps <= maxParentSteps ? relative : target;
    }

    // The inverse: a stored reference made absolute against the referring file's directory (not
    // normalized; see AssetManager::NormalizePath).
    [[nodiscard]]
    inline std::filesystem::path ResolvePortablePath(const std::filesystem::path& stored,
                                                     const std::filesystem::path& baseDirectory) {
        if (stored.empty() || stored.is_absolute()) { return stored; }
        return baseDirectory / stored;
    }

    // How asset files store the paths they carry (their source file, a mesh's texture paths):
    // relative to the project directory if the target is inside it, absolute otherwise. Unlike a
    // path relative to the asset itself, this stays correct when the asset is moved around inside
    // the project, and the whole project can move (or be checked out elsewhere). Both paths should
    // be absolute and normalized; with no project directory, the target is kept as it is.
    [[nodiscard]]
    inline std::filesystem::path MakeProjectPath(const std::filesystem::path& target,
                                                 const std::filesystem::path& projectDirectory) {
        if (target.empty() || projectDirectory.empty() || target.root_name() != projectDirectory.root_name()) {
            return target;
        }

        const std::filesystem::path relative = target.lexically_relative(projectDirectory);
        if (relative.empty() || *relative.begin() == "..") { return target; }
        return relative;
    }

    // The inverse (not normalized). A relative path with no project directory to resolve against is
    // returned as it is.
    [[nodiscard]]
    inline std::filesystem::path ResolveProjectPath(const std::filesystem::path& stored,
                                                    const std::filesystem::path& projectDirectory) {
        if (stored.empty() || stored.is_absolute() || projectDirectory.empty()) { return stored; }
        return projectDirectory / stored;
    }

}  // namespace Frost
