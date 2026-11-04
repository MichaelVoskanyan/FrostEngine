#pragma once

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// A navigable listing of one directory: a path bar (editable, so a full path can be typed or
// pasted), Up/Refresh buttons, and the directory's folders and files. Used embedded in the Asset
// Library panel and inside the editor's file dialogs.
class FileBrowser final {
public:
    // Decides which files are listed (directories always are). Null = every file.
    using FileFilter = std::function<bool(const std::filesystem::path&)>;

    struct Result {
        std::optional<std::filesystem::path> clicked;    // A file was selected this frame.
        std::optional<std::filesystem::path> activated;  // A file was double-clicked this frame.
    };

    explicit FileBrowser(std::filesystem::path directory, FileFilter filter = nullptr);

    // Draws the browser filling `size` (ImGui conventions: 0 = fill available, negative = fill
    // minus). Frost asset files for which `draggable` returns true can be dragged out (see
    // EditorUI); with a `draggable`, the other asset files say they cannot be used (they are not
    // part of the project). Null = nothing can be dragged.
    Result Draw(const char* id, const ImVec2& size, const FileFilter& draggable = nullptr);

    [[nodiscard]]
    const std::filesystem::path& GetDirectory() const noexcept;

    // Navigates to `directory` (or, for a file path, its directory, selecting the file).
    void Navigate(const std::filesystem::path& path);

    [[nodiscard]]
    const std::optional<std::filesystem::path>& GetSelectedFile() const noexcept;

private:
    struct Entry {
        std::filesystem::path path;
        std::string label;  // UTF-8 file name.
        bool isDirectory = false;
    };

    void Refresh();

    std::filesystem::path m_Directory;
    FileFilter m_Filter;
    std::vector<Entry> m_Entries;
    std::optional<std::filesystem::path> m_SelectedFile;
    std::string m_PathInput;
    std::string m_Error;  // Why the directory could not be listed, if it could not.
};
