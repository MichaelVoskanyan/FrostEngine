#include "FileBrowser.hpp"

#include "ImGuiHelpers.hpp"

#include <Frost/Assets/AssetImporter.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>

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

FileBrowser::FileBrowser(std::filesystem::path directory, FileFilter filter) : m_Filter(std::move(filter)) {
    Navigate(directory);
}

FileBrowser::Result FileBrowser::Draw(const char* const id, const ImVec2& size, const FileFilter& draggable) {
    Result result;

    ImGui::PushID(id);

    if (ImGui::Button("Up")) { Navigate(m_Directory.parent_path()); }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) { Refresh(); }
    ImGui::SameLine();

    // Editable path: Enter navigates (to a directory, or to a file's directory, selecting it).
    ImGui::SetNextItemWidth(-FLT_MIN);
    // Selecting everything on click makes typing or pasting a full path replace the current one.
    if (EditorUI::InputText("##Path", m_PathInput, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        const std::filesystem::path typed = Frost::PathFromUtf8(m_PathInput);
        std::error_code error;
        if (std::filesystem::is_regular_file(typed, error)) {
            Navigate(typed);
            result.clicked = m_SelectedFile;
        }
        else {
            Navigate(typed);
        }
    }

    if (ImGui::BeginChild("##Listing", size, ImGuiChildFlags_Borders)) {
        if (!m_Error.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_Error.c_str()); }

        // Navigation is deferred to after the loop: it replaces m_Entries.
        std::optional<std::filesystem::path> navigateTo;

        for (const Entry& entry : m_Entries) {
            ImGui::PushID(entry.label.c_str());

            if (entry.isDirectory) {
                if (ImGui::Selectable(("[Dir] " + entry.label).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    navigateTo = entry.path;
                }
            }
            else {
                // Project assets (draggable), source files that can be imported, and everything else
                // (dimmed).
                const std::optional<Frost::AssetType> type = Frost::AssetManager::GetAssetType(entry.path);
                const std::optional<Frost::AssetType> sourceType = Frost::AssetImporter::GetSourceType(entry.path);

                std::string label = entry.label;
                if (type.has_value()) { label = std::string(EditorUI::AssetTypeTag(*type)) + " " + entry.label; }
                else if (sourceType.has_value()) {
                    const char* const tag = sourceType == Frost::AssetType::Mesh    ? "[glTF] "
                                            : sourceType == Frost::AssetType::Sound ? "[Audio] "
                                                                                    : "[Image] ";
                    label = tag + entry.label;
                }

                const bool selected = m_SelectedFile == entry.path;
                const bool dimmed = !type.has_value() && !sourceType.has_value();

                if (dimmed) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); }

                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_SelectedFile = entry.path;
                    m_PathInput = Frost::PathToUtf8(entry.path);
                    result.clicked = entry.path;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { result.activated = entry.path; }
                }

                if (dimmed) { ImGui::PopStyleColor(); }

                if (type.has_value() && draggable != nullptr) {
                    if (draggable(entry.path)) { EditorUI::AssetDragSource(entry.path); }
                    else {
                        ImGui::SetItemTooltip("Not part of this project (it is outside its content folder), so it cannot be\n"
                                              "used here. Projects are self-contained: copy the file into the project's\n"
                                              "content folder (for a model, with the textures it uses) to use it.");
                    }
                }
                if (sourceType.has_value() && !type.has_value()) { ImGui::SetItemTooltip("Double-click to import."); }
            }

            ImGui::PopID();
        }

        if (navigateTo.has_value()) { Navigate(*navigateTo); }
    }
    ImGui::EndChild();

    ImGui::PopID();
    return result;
}

const std::filesystem::path& FileBrowser::GetDirectory() const noexcept { return m_Directory; }

void FileBrowser::Navigate(const std::filesystem::path& path) {
    const std::filesystem::path normalized = Frost::AssetManager::NormalizePath(path);

    std::error_code error;
    if (std::filesystem::is_regular_file(normalized, error)) {
        m_Directory = normalized.parent_path();
        m_SelectedFile = normalized;
    }
    else {
        m_Directory = normalized;
        m_SelectedFile.reset();
    }

    m_PathInput = Frost::PathToUtf8(m_SelectedFile.value_or(m_Directory));
    Refresh();
}

const std::optional<std::filesystem::path>& FileBrowser::GetSelectedFile() const noexcept { return m_SelectedFile; }

void FileBrowser::Refresh() {
    m_Entries.clear();
    m_Error.clear();

    std::error_code error;
    std::filesystem::directory_iterator it(m_Directory, std::filesystem::directory_options::skip_permission_denied,
                                           error);
    if (error) {
        m_Error = "Cannot list '" + Frost::PathToUtf8(m_Directory) + "': " + error.message();
        return;
    }

    for (const std::filesystem::directory_iterator end{}; it != end; it.increment(error)) {
        if (error) { break; }

        std::error_code entryError;
        const bool isDirectory = it->is_directory(entryError);
        if (!isDirectory && m_Filter != nullptr && !m_Filter(it->path())) { continue; }

        m_Entries.push_back(Entry{.path = it->path(), .label = Frost::PathToUtf8(it->path().filename()), .isDirectory = isDirectory});
    }

    // Folders first, then case-insensitively by name.
    std::sort(m_Entries.begin(), m_Entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDirectory != b.isDirectory) { return a.isDirectory; }
        return Lowercase(a.label) < Lowercase(b.label);
    });
}
