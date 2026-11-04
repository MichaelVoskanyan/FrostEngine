#pragma once

#include "FileBrowser.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

// A modal open/save dialog for one file type, built on FileBrowser. In Save mode a file name (or
// a full path) is typed, and the extension is added if left off.
class FileDialog final {
public:
    enum class Mode { Open, Save };

    // `title` must be unique among open popups; `extension` includes the dot (".flevel").
    FileDialog(std::string title, Mode mode, std::string extension, const std::filesystem::path& startDirectory);

    // The browser's filter refers back to this object.
    FileDialog(const FileDialog&) = delete;
    FileDialog& operator=(const FileDialog&) = delete;
    FileDialog(FileDialog&&) = delete;
    FileDialog& operator=(FileDialog&&) = delete;

    // Says what is wrong with a chosen path (empty = nothing); the dialog shows it and will not
    // confirm such a path.
    using Validator = std::function<std::string(const std::filesystem::path&)>;

    void SetValidator(Validator validator);

    // Opens the dialog on the next Draw(). An existing `initialPath` (e.g. the current level's
    // file) is selected/named and the dialog starts in its directory; otherwise it starts in
    // `directory`, with `fileName` filled in (Save mode).
    void Open(const std::optional<std::filesystem::path>& initialPath, const std::filesystem::path& directory,
              const std::string& fileName = {});

    // Call every frame, outside any other window/popup. Returns the chosen path on the frame the
    // user confirms; nothing otherwise (including on cancel).
    [[nodiscard]]
    std::optional<std::filesystem::path> Draw();

    // True from Open() until the user confirms or cancels.
    [[nodiscard]]
    bool IsOpen() const noexcept;

private:
    [[nodiscard]]
    bool MatchesExtension(const std::filesystem::path& path) const;

    // The path the current input describes (Save mode), with the extension ensured.
    [[nodiscard]]
    std::filesystem::path ResolveSavePath() const;

    std::string m_Title;
    Mode m_Mode;
    std::string m_Extension;
    FileBrowser m_Browser;
    Validator m_Validator;
    std::string m_FileName;
    bool m_RequestOpen = false;
    bool m_Open = false;
};
