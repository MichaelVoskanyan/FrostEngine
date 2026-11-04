#include <Frost/Project/Project.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>
#include <vector>

namespace {

    [[nodiscard]]
    bool HasDescriptorExtension(const std::filesystem::path& path) {
        std::string extension = Frost::PathToUtf8(path.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == Frost::Project::kDescriptorExtension;
    }

    // The .frostproject files directly in `directory`, sorted.
    [[nodiscard]]
    std::vector<std::filesystem::path> FindDescriptors(const std::filesystem::path& directory) {
        std::vector<std::filesystem::path> descriptors;

        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            std::error_code entryError;
            if (it->is_regular_file(entryError) && HasDescriptorExtension(it->path())) { descriptors.push_back(it->path()); }
        }

        std::sort(descriptors.begin(), descriptors.end());
        return descriptors;
    }

    // `path` is `directory` or inside it (both normalized).
    [[nodiscard]]
    bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& directory) {
        if (path == directory) { return true; }
        const std::filesystem::path relative = path.lexically_relative(directory);
        return !relative.empty() && *relative.begin() != ".." && *relative.begin() != ".";
    }

}  // namespace

namespace Frost {

    Project::Project(const std::filesystem::path& rootDirectory)
        : m_Root(AssetManager::NormalizePath(rootDirectory.empty() ? std::filesystem::path(".") : rootDirectory)) {
        const std::vector<std::filesystem::path> descriptors = FindDescriptors(m_Root);
        if (descriptors.size() == 1) { m_Descriptor = AssetManager::NormalizePath(descriptors.front()); }

        m_Name = PathToUtf8(m_Descriptor.has_value() ? m_Descriptor->stem() : m_Root.filename());
    }

    Project Project::Open(const std::filesystem::path& path) {
        const std::filesystem::path normalized = AssetManager::NormalizePath(path);
        std::error_code error;

        // A descriptor: its directory is the project.
        if (std::filesystem::is_regular_file(normalized, error)) {
            if (!HasDescriptorExtension(normalized)) {
                throw ProjectError("'" + PathToUtf8(normalized) + "' is not a project: expected a project directory or a " +
                                   kDescriptorExtension + " file.");
            }
            Project project(normalized.parent_path());
            project.m_Descriptor = normalized;
            project.m_Name = PathToUtf8(normalized.stem());
            return project;
        }

        if (!std::filesystem::is_directory(normalized, error)) {
            throw ProjectError("'" + PathToUtf8(normalized) + "' does not exist.");
        }

        const std::vector<std::filesystem::path> descriptors = FindDescriptors(normalized);
        if (descriptors.size() > 1) {
            throw ProjectError("'" + PathToUtf8(normalized) + "' contains several " + kDescriptorExtension +
                               " files; pass the one to open.");
        }
        if (!IsProjectDirectory(normalized)) {
            throw ProjectError("'" + PathToUtf8(normalized) + "' is not a Frost project: it has no '" +
                               kContentDirectoryName + "' directory and no " + kDescriptorExtension +
                               " file. To start a new project there, create an empty '" + kContentDirectoryName +
                               "' directory in it.");
        }
        return Project(normalized);
    }

    bool Project::IsProjectDirectory(const std::filesystem::path& directory) {
        std::error_code error;
        return std::filesystem::is_directory(directory / kContentDirectoryName, error) || !FindDescriptors(directory).empty();
    }

    const std::string& Project::GetName() const noexcept { return m_Name; }

    const std::filesystem::path& Project::GetRootDirectory() const noexcept { return m_Root; }

    const std::optional<std::filesystem::path>& Project::GetDescriptorPath() const noexcept { return m_Descriptor; }

    std::filesystem::path Project::GetContentDirectory() const { return m_Root / kContentDirectoryName; }

    std::filesystem::path Project::GetLevelsDirectory() const { return GetContentDirectory() / kLevelsDirectoryName; }

    std::filesystem::path Project::GetSourceDirectory() const { return m_Root / kSourceDirectoryName; }

    bool Project::IsInContent(const std::filesystem::path& path) const {
        return IsWithin(AssetManager::NormalizePath(path), AssetManager::NormalizePath(GetContentDirectory()));
    }

    bool Project::IsInProject(const std::filesystem::path& path) const {
        return IsWithin(AssetManager::NormalizePath(path), m_Root);
    }

    void Project::CreateDirectories() const {
        std::error_code error;
        std::filesystem::create_directories(GetLevelsDirectory(), error);
        if (error) {
            throw ProjectError("Could not create the project's directories in '" + PathToUtf8(m_Root) + "': " +
                               error.message());
        }
    }

}  // namespace Frost
