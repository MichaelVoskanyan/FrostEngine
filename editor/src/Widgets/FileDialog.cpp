#include "FileDialog.hpp"

#include "ImGuiHelpers.hpp"

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace {

    [[nodiscard]]
    std::string Lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

}  // namespace

FileDialog::FileDialog(std::string title, const Mode mode, std::string extension,
                       const std::filesystem::path& startDirectory)
    : m_Title(std::move(title)),
      m_Mode(mode),
      m_Extension(Lowercase(std::move(extension))),
      m_Browser(startDirectory, [this](const std::filesystem::path& path) { return MatchesExtension(path); }) {}

void FileDialog::SetValidator(Validator validator) { m_Validator = std::move(validator); }

void FileDialog::Open(const std::optional<std::filesystem::path>& initialPath, const std::filesystem::path& directory,
                      const std::string& fileName) {
    m_RequestOpen = true;
    m_Open = true;

    std::error_code error;
    if (initialPath.has_value() && std::filesystem::is_regular_file(*initialPath, error)) {
        m_Browser.Navigate(*initialPath);
        m_FileName = Frost::PathToUtf8(initialPath->filename());
    }
    else {
        m_Browser.Navigate(directory);
        m_FileName = m_Mode == Mode::Save ? fileName : std::string();
        if (m_Mode == Mode::Save && !m_FileName.empty() && !MatchesExtension(Frost::PathFromUtf8(m_FileName))) {
            m_FileName += m_Extension;
        }
    }
}

std::optional<std::filesystem::path> FileDialog::Draw() {
    if (m_RequestOpen) {
        ImGui::OpenPopup(m_Title.c_str());
        m_RequestOpen = false;
    }

    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 460.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(m_Title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        m_Open = false;
        return std::nullopt;
    }

    std::optional<std::filesystem::path> chosen;
    bool confirm = false;

    // Input, status and buttons, plus room for a validation message.
    const float footer = ImGui::GetFrameHeightWithSpacing() * (m_Mode == Mode::Save ? 3.0f : 2.0f) +
                         (m_Validator != nullptr ? ImGui::GetTextLineHeightWithSpacing() * 2.0f : 0.0f);
    const FileBrowser::Result browsed = m_Browser.Draw("Browser", ImVec2(0.0f, -footer));
    if (browsed.clicked.has_value()) { m_FileName = Frost::PathToUtf8(browsed.clicked->filename()); }
    if (browsed.activated.has_value()) { confirm = true; }

    std::filesystem::path target;
    if (m_Mode == Mode::Save) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); }
        confirm |= EditorUI::InputText("##FileName", m_FileName, ImGuiInputTextFlags_EnterReturnsTrue);

        target = ResolveSavePath();
        std::error_code error;
        if (!m_FileName.empty() && std::filesystem::exists(target, error)) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s already exists and will be replaced.",
                               Frost::PathToUtf8(target.filename()).c_str());
        }
        else {
            ImGui::TextDisabled("%s", m_FileName.empty() ? "Enter a file name." : Frost::PathToUtf8(target).c_str());
        }
    }
    else {
        target = m_Browser.GetSelectedFile().value_or(std::filesystem::path{});
        ImGui::TextDisabled("%s", target.empty() ? "Select a file." : Frost::PathToUtf8(target).c_str());
    }

    bool valid = m_Mode == Mode::Save ? !m_FileName.empty() : !target.empty();
    if (valid && m_Validator != nullptr) {
        if (const std::string problem = m_Validator(target); !problem.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", problem.c_str());
            ImGui::PopTextWrapPos();
            valid = false;
        }
    }

    ImGui::BeginDisabled(!valid);
    confirm |= ImGui::Button(m_Mode == Mode::Save ? "Save" : "Open", ImVec2(120.0f, 0.0f));
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (confirm && valid) {
        chosen = target;
        m_Open = false;
        ImGui::CloseCurrentPopup();
    }
    else if (cancel) {
        m_Open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return chosen;
}

bool FileDialog::IsOpen() const noexcept { return m_Open; }

bool FileDialog::MatchesExtension(const std::filesystem::path& path) const {
    return Lowercase(Frost::PathToUtf8(path.extension())) == m_Extension;
}

std::filesystem::path FileDialog::ResolveSavePath() const {
    std::filesystem::path path = Frost::PathFromUtf8(m_FileName);
    if (!path.is_absolute()) { path = m_Browser.GetDirectory() / path; }
    if (!MatchesExtension(path)) { path += m_Extension; }
    return Frost::AssetManager::NormalizePath(path);
}
