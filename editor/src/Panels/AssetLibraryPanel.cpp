#include "AssetLibraryPanel.hpp"

#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Assets/AssetImporter.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Scene/Level.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <optional>
#include <system_error>

namespace {

    constexpr ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
    constexpr ImVec4 kWarningColor(1.0f, 0.8f, 0.3f, 1.0f);

    [[nodiscard]]
    std::string Lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    [[nodiscard]]
    bool ContainsCaseInsensitive(const std::string& text, const std::string& query) {
        return query.empty() || Lowercase(text).find(Lowercase(query)) != std::string::npos;
    }

    [[nodiscard]]
    bool IsImportableSource(const std::filesystem::path& path) {
        return Frost::AssetImporter::GetSourceType(path).has_value();
    }

    [[nodiscard]]
    bool FileExists(const std::filesystem::path& path) {
        std::error_code error;
        return !path.empty() && std::filesystem::is_regular_file(path, error);
    }

    // A plain file name (no folders, nothing reserved).
    [[nodiscard]]
    bool IsValidAssetName(const std::string& name) {
        if (name.empty() || name == "." || name == "..") { return false; }
        return name.find_first_of("/\\:*?\"<>|") == std::string::npos;
    }

    [[nodiscard]]
    std::string FormatSize(const std::uintmax_t bytes) {
        char text[32];
        if (bytes >= 1024 * 1024) { std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0)); }
        else { std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0); }
        return text;
    }

    // Where assets of a type go when imported while browsing `current`: a "Meshes"/"Textures"
    // folder there (Weapons/Glock19 -> Weapons/Glock19/Meshes), or `current` itself (or its
    // sibling) if it already is one.
    [[nodiscard]]
    std::filesystem::path DefaultImportFolder(const std::filesystem::path& current, const Frost::AssetType type) {
        const std::string name = Lowercase(Frost::PathToUtf8(current.filename()));
        const std::string wanted = type == Frost::AssetType::Mesh ? "Meshes" : "Textures";
        const std::string other = type == Frost::AssetType::Mesh ? "textures" : "meshes";

        if (name == Lowercase(wanted)) { return current; }
        if (name == other) { return current.parent_path() / wanted; }
        return current / wanted;
    }

}  // namespace

AssetLibraryPanel::AssetLibraryPanel(const std::filesystem::path& startDirectory)
    : m_FileSystem(startDirectory), m_ImportBrowser(startDirectory, IsImportableSource) {}

void AssetLibraryPanel::Draw(EditorContext& context) {
    AssetLibrary& library = context.GetLibrary();

    // The current folder can disappear under us (deleted, renamed, moved in the OS).
    std::error_code error;
    if (m_CurrentFolder.empty() || !library.IsInside(m_CurrentFolder) ||
        !std::filesystem::is_directory(m_CurrentFolder, error)) {
        m_CurrentFolder = library.GetContentDirectory();
    }

    if (ImGui::Begin(kWindowName)) {
        if (ImGui::Button("Import...")) { OpenImportDialog(context, {}); }
        ImGui::SetItemTooltip("Import a model or image into the current folder.");
        ImGui::SameLine();
        if (ImGui::Button("New Folder")) { RequestNewFolder(m_CurrentFolder); }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) { m_Deferred.emplace_back([&context] { context.GetLibrary().Refresh(); context.OnAssetsChanged(); }); }
        ImGui::SetItemTooltip("Rescan the content directory now (it is also rescanned every second).");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        EditorUI::InputText("##Filter", m_Filter);
        if (m_Filter.empty() && !ImGui::IsItemActive()) {
            // Placeholder text, drawn over the empty field.
            const ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + ImGui::GetStyle().FramePadding.x, min.y + ImGui::GetStyle().FramePadding.y),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), "Search all folders...");
        }

        if (ImGui::BeginTabBar("##Tabs")) {
            if (ImGui::BeginTabItem("Library")) {
                DrawLibraryTab(context);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("File System")) {
                DrawFileSystemTab(context);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Lights")) {
                DrawLightsTab(context);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    // Now that nothing refers into the library's lists any more.
    std::vector<std::function<void()>> deferred = std::move(m_Deferred);
    m_Deferred.clear();
    for (const std::function<void()>& operation : deferred) { operation(); }

    DrawNamePrompt(context);
    DrawDeletePrompt(context);
    DrawImportDialog(context);
}

void AssetLibraryPanel::DrawLibraryTab(EditorContext& context) {
    AssetLibrary& library = context.GetLibrary();

    // Left: the folder tree, resizable.
    if (ImGui::BeginChild("##FolderTree", ImVec2(ImGui::GetFontSize() * 16.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
        DrawFolderTree(context, library.GetContentDirectory());
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: where we are, and what is there.
    if (ImGui::BeginChild("##FolderContents", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        DrawBreadcrumbs(context);
        DrawFolderContents(context);

        // Right-click on empty space.
        if (ImGui::BeginPopupContextWindow("##ContentsMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("New Folder...")) { RequestNewFolder(m_CurrentFolder); }
            if (ImGui::MenuItem("Import Here...")) { OpenImportDialog(context, {}); }
            ImGui::Separator();
            if (ImGui::MenuItem("Show in File System")) { m_FileSystem.Navigate(m_CurrentFolder); }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
}

void AssetLibraryPanel::DrawFolderTree(EditorContext& context, const std::filesystem::path& folder) {
    AssetLibrary& library = context.GetLibrary();
    const std::vector<std::filesystem::path>& children = library.GetSubfolders(folder);
    const bool isRoot = folder == library.GetContentDirectory();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) { flags |= ImGuiTreeNodeFlags_Leaf; }
    if (folder == m_CurrentFolder) { flags |= ImGuiTreeNodeFlags_Selected; }
    if (isRoot) { flags |= ImGuiTreeNodeFlags_DefaultOpen; }

    // Keep the current folder visible: open its ancestors.
    const std::filesystem::path relative = m_CurrentFolder.lexically_relative(folder);
    if (!isRoot && folder != m_CurrentFolder && !relative.empty() && *relative.begin() != "..") {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }

    ImGui::PushID(Frost::PathToUtf8(folder).c_str());
    const std::string label = isRoot ? "Content" : Frost::PathToUtf8(folder.filename());
    const bool open = ImGui::TreeNodeEx("##Folder", flags, "%s", label.c_str());

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) { m_CurrentFolder = folder; }
    if (!isRoot && folder != library.GetLevelsDirectory()) { EditorUI::FolderDragSource(folder); }
    AcceptMoveInto(context, folder);
    if (ImGui::BeginPopupContextItem()) {
        DrawFolderMenu(context, folder);
        ImGui::EndPopup();
    }

    if (open) {
        for (const std::filesystem::path& child : children) { DrawFolderTree(context, child); }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetLibraryPanel::DrawBreadcrumbs(EditorContext& context) {
    AssetLibrary& library = context.GetLibrary();

    // Content > Weapons > Glock19: each part opens that folder, and takes drops.
    std::vector<std::filesystem::path> chain{library.GetContentDirectory()};
    for (const std::filesystem::path& part : m_CurrentFolder.lexically_relative(library.GetContentDirectory())) {
        if (part == ".") { continue; }
        chain.push_back(chain.back() / part);
    }

    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextDisabled(">");
            ImGui::SameLine(0.0f, 2.0f);
        }

        ImGui::PushID(static_cast<int>(i));
        const std::string label = i == 0 ? "Content" : Frost::PathToUtf8(chain[i].filename());
        if (ImGui::SmallButton(label.c_str())) { m_CurrentFolder = chain[i]; }
        AcceptMoveInto(context, chain[i]);
        ImGui::PopID();
    }
    ImGui::Separator();
}

void AssetLibraryPanel::DrawFolderContents(EditorContext& context) {
    AssetLibrary& library = context.GetLibrary();
    const bool searching = !m_Filter.empty();

    // (Above the table, which takes all the space there is.)
    if (!searching && library.GetSubfolders(m_CurrentFolder).empty() && library.GetAssetsIn(m_CurrentFolder).empty() &&
        library.GetLevelsIn(m_CurrentFolder).empty()) {
        ImGui::TextDisabled("%s", m_CurrentFolder == library.GetLevelsDirectory()
                                          ? "No levels yet. File > Save Level saves the level being edited here."
                                          : "Empty. Import here, or drag assets in (right-click for options).");
    }

    constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##Contents", searching ? 5 : 4, kTableFlags)) { return; }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
    if (searching) { ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 0.25f); }
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
    ImGui::TableSetupColumn("Imported From", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableHeadersRow();

    if (searching) {
        // Every asset whose name matches, wherever it is.
        for (const AssetLibrary::Entry& entry : library.GetEntries()) {
            if (ContainsCaseInsensitive(Frost::PathToUtf8(entry.path.stem()), m_Filter)) { DrawAssetRow(context, entry, true); }
        }
        for (const std::filesystem::path& level : library.GetAllLevels()) {
            if (ContainsCaseInsensitive(Frost::PathToUtf8(level.stem()), m_Filter)) { DrawLevelRow(context, level, true); }
        }
    }
    else {
        for (const std::filesystem::path& folder : library.GetSubfolders(m_CurrentFolder)) { DrawFolderRow(context, folder); }
        for (const std::filesystem::path& level : library.GetLevelsIn(m_CurrentFolder)) { DrawLevelRow(context, level, false); }
        for (const AssetLibrary::Entry* const entry : library.GetAssetsIn(m_CurrentFolder)) { DrawAssetRow(context, *entry, false); }
    }

    ImGui::EndTable();
}

void AssetLibraryPanel::DrawFolderRow(EditorContext& context, const std::filesystem::path& folder) {
    ImGui::PushID(Frost::PathToUtf8(folder).c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    const std::string label = "[Folder] " + Frost::PathToUtf8(folder.filename());
    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_CurrentFolder = folder;
    }
    if (folder != context.GetLibrary().GetLevelsDirectory()) { EditorUI::FolderDragSource(folder); }
    AcceptMoveInto(context, folder);
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Open")) { m_CurrentFolder = folder; }
        DrawFolderMenu(context, folder);
        ImGui::EndPopup();
    }

    ImGui::TableNextColumn();
    ImGui::TextDisabled("Folder");
    ImGui::PopID();
}

void AssetLibraryPanel::DrawAssetRow(EditorContext& context, const AssetLibrary::Entry& entry, const bool showFolder) {
    const bool sourceExists = FileExists(entry.sourcePath);
    const bool partOfModel = !entry.sourceItem.empty();  // e.g. a texture a model came with.

    ImGui::PushID(Frost::PathToUtf8(entry.path).c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    const std::string name = Frost::PathToUtf8(entry.path.stem());
    if (entry.duplicate) { ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor); }
    const bool activated = ImGui::Selectable(name.c_str(), false,
                                             ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                           ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (entry.duplicate) { ImGui::PopStyleColor(); }

    EditorUI::AssetDragSource(entry.path);
    if (entry.duplicate) {
        ImGui::SetItemTooltip("%s\nThis file has the same asset id as another one (a copy made outside the editor?).\n"
                              "References go to the other one. Right-click > Give Unique Id to make this a separate asset.",
                              Frost::PathToUtf8(entry.path).c_str());
    }
    else {
        ImGui::SetItemTooltip("%s\n%s", Frost::PathToUtf8(entry.path).c_str(),
                              entry.type == Frost::AssetType::Mesh ? "Drag into the viewport to place it, or onto a folder to move it."
                                                                   : "Drag onto a texture slot in the Inspector, or onto a folder to move it.");
    }

    // Double-clicking a model places it in front of the camera.
    if (activated && entry.type == Frost::AssetType::Mesh) { context.SpawnMesh(entry.path, context.GetSpawnPoint()); }

    if (ImGui::BeginPopupContextItem()) {
        const std::filesystem::path path = entry.path;
        if (entry.type == Frost::AssetType::Mesh && ImGui::MenuItem("Add to Level")) {
            context.SpawnMesh(path, context.GetSpawnPoint());
        }

        const bool canReimport = sourceExists && !partOfModel;
        if (ImGui::MenuItem("Reimport", nullptr, false, canReimport)) {
            m_Deferred.emplace_back([&context, path] { context.ReimportAsset(path); });
        }
        if (partOfModel) {
            ImGui::SetItemTooltip("Imported as part of %s; reimport that model to update it.",
                                  Frost::PathToUtf8(entry.sourcePath.filename()).c_str());
        }
        else if (!sourceExists) {
            ImGui::SetItemTooltip("The source file is gone (renamed, moved or deleted); use Reimport From...");
        }
        if (!partOfModel && ImGui::MenuItem("Reimport From...")) { OpenImportDialog(context, {}, path); }

        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) { RequestRename(path); }
        if (ImGui::MenuItem("Duplicate")) {
            m_Deferred.emplace_back([&context, path] {
                if (const std::string error = context.GetLibrary().Duplicate(path); !error.empty()) { context.ReportError(error); }
            });
        }
        if (entry.duplicate && ImGui::MenuItem("Give Unique Id")) {
            m_Deferred.emplace_back([&context, path] {
                if (const std::string error = context.GetLibrary().MakeUnique(path); !error.empty()) { context.ReportError(error); }
            });
        }
        if (ImGui::MenuItem("Delete...")) { RequestDelete(context, path); }

        ImGui::Separator();
        if (ImGui::MenuItem("Show Folder")) { m_CurrentFolder = path.parent_path(); m_Filter.clear(); }
        if (ImGui::MenuItem("Show in File System")) { m_FileSystem.Navigate(path); }
        if (ImGui::MenuItem("Show Source in File System", nullptr, false, sourceExists)) { m_FileSystem.Navigate(entry.sourcePath); }
        ImGui::EndPopup();
    }

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(entry.type == Frost::AssetType::Mesh ? "Model" : "Texture");

    if (showFolder) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", DisplayFolder(context, entry.path.parent_path()).c_str());
    }

    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", FormatSize(entry.size).c_str());

    ImGui::TableNextColumn();
    if (entry.sourcePath.empty()) { ImGui::TextDisabled("(unknown)"); }
    else {
        std::string source = Frost::PathToUtf8(entry.sourcePath.filename());
        if (partOfModel) { source += " > " + entry.sourceItem.substr(entry.sourceItem.find_first_of(":#") + 1); }

        // A missing source is fine (the asset is self-contained); it only means no reimport.
        if (!sourceExists) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); }
        ImGui::TextUnformatted(source.c_str());
        if (!sourceExists) { ImGui::PopStyleColor(); }
        ImGui::SetItemTooltip("%s%s", Frost::PathToUtf8(entry.sourcePath).c_str(),
                              sourceExists ? "" : "\n(not found: the asset works, but can only be reimported from another file)");
    }

    ImGui::PopID();
}

void AssetLibraryPanel::DrawLevelRow(EditorContext& context, const std::filesystem::path& level, const bool showFolder) {
    ImGui::PushID(Frost::PathToUtf8(level).c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    const bool isOpen = context.GetLevelPath() == level;
    std::string name = Frost::PathToUtf8(level.stem());
    if (isOpen) { name += "  (open)"; }

    if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        context.RequestOpenLevel(level);
    }
    ImGui::SetItemTooltip("%s\nDouble-click to open it.", Frost::PathToUtf8(level).c_str());

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Open", nullptr, false, !isOpen)) { context.RequestOpenLevel(level); }
        ImGui::Separator();
        if (ImGui::MenuItem("Show Folder")) {
            m_CurrentFolder = level.parent_path();
            m_Filter.clear();
        }
        if (ImGui::MenuItem("Show in File System")) { m_FileSystem.Navigate(level); }
        ImGui::EndPopup();
    }

    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Level");

    if (showFolder) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", DisplayFolder(context, level.parent_path()).c_str());
    }

    ImGui::PopID();
}

void AssetLibraryPanel::DrawFolderMenu(EditorContext& context, const std::filesystem::path& folder) {
    // The content root and the project's Levels folder stay where they are.
    const bool isRoot = folder == context.GetLibrary().GetContentDirectory() || folder == context.GetLibrary().GetLevelsDirectory();

    if (ImGui::MenuItem("New Folder...")) { RequestNewFolder(folder); }
    if (ImGui::MenuItem("Import Here...")) {
        m_CurrentFolder = folder;
        OpenImportDialog(context, {});
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rename...", nullptr, false, !isRoot)) { RequestRename(folder); }
    if (ImGui::MenuItem("Delete...", nullptr, false, !isRoot)) { RequestDelete(context, folder); }
    ImGui::Separator();
    if (ImGui::MenuItem("Show in File System")) { m_FileSystem.Navigate(folder); }
}

void AssetLibraryPanel::AcceptMoveInto(EditorContext& context, const std::filesystem::path& folder) {
    if (!ImGui::BeginDragDropTarget()) { return; }

    if (const std::optional<std::filesystem::path> item = EditorUI::AcceptMovePayload()) {
        const std::filesystem::path source = *item;
        m_Deferred.emplace_back([this, &context, source, folder] {
            std::filesystem::path moved;
            if (const std::string error = context.GetLibrary().Move(source, folder, &moved); !error.empty()) {
                context.ReportError(error);
                return;
            }

            // Follow a moved folder that was being browsed (or that contained it).
            const std::filesystem::path relative = m_CurrentFolder.lexically_relative(source);
            if (!moved.empty() && !relative.empty() && *relative.begin() != "..") {
                m_CurrentFolder = (moved / relative).lexically_normal();
            }
        });
    }
    ImGui::EndDragDropTarget();
}

void AssetLibraryPanel::RequestNewFolder(const std::filesystem::path& parent) {
    m_NamePrompt = NamePrompt{.kind = NamePrompt::Kind::NewFolder, .target = parent, .name = "New Folder", .error = {}};
    m_OpenNamePrompt = true;
}

void AssetLibraryPanel::RequestRename(const std::filesystem::path& item) {
    std::error_code error;
    const bool isFolder = std::filesystem::is_directory(item, error);
    m_NamePrompt = NamePrompt{.kind = NamePrompt::Kind::Rename,
                              .target = item,
                              .name = Frost::PathToUtf8(isFolder ? item.filename() : item.stem()),
                              .error = {}};
    m_OpenNamePrompt = true;
}

void AssetLibraryPanel::RequestDelete(EditorContext& context, const std::filesystem::path& item) {
    AssetLibrary& library = context.GetLibrary();

    std::error_code error;
    DeletePrompt prompt{.target = item, .isFolder = std::filesystem::is_directory(item, error), .referencers = {},
                        .usesInOpenLevel = 0, .error = {}};

    if (!prompt.isFolder) {
        if (const AssetLibrary::Entry* const entry = library.FindByPath(item); entry != nullptr && entry->id.IsValid()) {
            prompt.referencers = library.FindReferencers(entry->id);

            // The open level may have unsaved uses the files do not show.
            for (const std::unique_ptr<Frost::GameObject>& object : context.GetLevel().GetObjects()) {
                bool uses = object->GetMeshReference().id == entry->id;
                for (std::size_t i = 0; i < object->GetMaterialSlotCount() && !uses; ++i) {
                    if (const auto& materialOverride = object->GetMaterialOverride(i); materialOverride.has_value()) {
                        for (const Frost::TextureOverride& texture : materialOverride->textures) {
                            uses = uses || (texture.source == Frost::TextureOverride::Source::Asset && texture.texture.id == entry->id);
                        }
                    }
                }
                prompt.usesInOpenLevel += uses ? 1 : 0;
            }
        }
    }

    m_DeletePrompt = std::move(prompt);
    m_OpenDeletePrompt = true;
}

void AssetLibraryPanel::DrawNamePrompt(EditorContext& context) {
    constexpr const char* kPopupName = "Name##AssetLibrary";
    if (m_OpenNamePrompt) {
        ImGui::OpenPopup(kPopupName);
        m_OpenNamePrompt = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kPopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    if (!m_NamePrompt.has_value()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    NamePrompt& prompt = *m_NamePrompt;
    const bool newFolder = prompt.kind == NamePrompt::Kind::NewFolder;
    ImGui::Text("%s", newFolder ? ("New folder in " + DisplayFolder(context, prompt.target) + ":").c_str()
                                : ("Rename '" + Frost::PathToUtf8(prompt.target.filename()) + "' to:").c_str());

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
    if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); }
    bool submit = EditorUI::InputText("##Name", prompt.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

    if (!prompt.error.empty()) { ImGui::TextColored(kErrorColor, "%s", prompt.error.c_str()); }
    else if (!newFolder) { ImGui::TextDisabled("References to it are unaffected."); }

    submit |= ImGui::Button(newFolder ? "Create" : "Rename", ImVec2(110.0f, 0.0f));
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (submit) {
        AssetLibrary& library = context.GetLibrary();
        std::filesystem::path result;
        prompt.error = newFolder ? library.CreateFolder(prompt.target, prompt.name, &result)
                                 : library.Rename(prompt.target, prompt.name, &result);

        if (prompt.error.empty()) {
            if (newFolder) { m_CurrentFolder = result; }
            else {
                // Keep browsing the same place if it was the renamed folder (or inside it).
                const std::filesystem::path relative = m_CurrentFolder.lexically_relative(prompt.target);
                if (!relative.empty() && *relative.begin() != "..") { m_CurrentFolder = (result / relative).lexically_normal(); }
            }
            m_NamePrompt.reset();
            ImGui::CloseCurrentPopup();
        }
    }
    else if (cancel) {
        m_NamePrompt.reset();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void AssetLibraryPanel::DrawDeletePrompt(EditorContext& context) {
    constexpr const char* kPopupName = "Delete##AssetLibrary";
    if (m_OpenDeletePrompt) {
        ImGui::OpenPopup(kPopupName);
        m_OpenDeletePrompt = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(900.0f, 600.0f));
    if (!ImGui::BeginPopupModal(kPopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    if (!m_DeletePrompt.has_value()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    DeletePrompt& prompt = *m_DeletePrompt;
    AssetLibrary& library = context.GetLibrary();
    const std::string name = Frost::PathToUtf8(prompt.target.filename());

    bool canDelete = true;
    if (prompt.isFolder) {
        if (library.ContainsAssets(prompt.target)) {
            ImGui::Text("'%s' still contains assets.", name.c_str());
            ImGui::TextDisabled("Move or delete them first.");
            canDelete = false;
        }
        else {
            ImGui::Text("Delete the folder '%s'?", name.c_str());
        }
    }
    else {
        ImGui::Text("Delete '%s'? This cannot be undone.", name.c_str());

        if (prompt.referencers.empty() && prompt.usesInOpenLevel == 0) {
            ImGui::TextDisabled("Nothing in the project refers to it.");
        }
        else {
            ImGui::TextColored(kWarningColor, "It is still in use; those references will show as missing:");
            if (prompt.usesInOpenLevel > 0) { ImGui::BulletText("%d object(s) in the open level", prompt.usesInOpenLevel); }
            for (const std::filesystem::path& referencer : prompt.referencers) {
                ImGui::BulletText("%s", Frost::GenericPathToUtf8(referencer.lexically_relative(library.GetContentDirectory())).c_str());
            }
        }
    }

    if (!prompt.error.empty()) { ImGui::TextColored(kErrorColor, "%s", prompt.error.c_str()); }

    ImGui::Spacing();
    ImGui::BeginDisabled(!canDelete);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.1f, 1.0f));
    const bool confirm = ImGui::Button("Delete", ImVec2(110.0f, 0.0f));
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button(canDelete ? "Cancel" : "Close", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (confirm && canDelete) {
        prompt.error = prompt.isFolder ? library.DeleteFolder(prompt.target) : context.DeleteAsset(prompt.target);
        if (prompt.error.empty()) {
            m_DeletePrompt.reset();
            ImGui::CloseCurrentPopup();
        }
    }
    else if (cancel) {
        m_DeletePrompt.reset();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void AssetLibraryPanel::DrawFileSystemTab(EditorContext& context) {
    // Only the project's own assets can be dragged into the level; the project never uses files
    // from elsewhere (e.g. another project's content).
    const AssetLibrary& library = context.GetLibrary();
    const FileBrowser::Result result = m_FileSystem.Draw(
            "FileSystem", ImVec2(0.0f, 0.0f), [&library](const std::filesystem::path& path) { return library.IsInside(path); });

    // Double-clicking a source file offers to import it (into the Library's current folder).
    if (result.activated.has_value() && IsImportableSource(*result.activated)) {
        OpenImportDialog(context, *result.activated);
    }
}

void AssetLibraryPanel::DrawLightsTab(EditorContext& context) {
    struct Entry {
        Frost::LightType type;
        const char* description;
    };
    constexpr Entry kEntries[] = {
            {Frost::LightType::Directional, "Sunlight: parallel rays from one direction, everywhere in the level."},
            {Frost::LightType::Point, "A bulb: shines in all directions from a point, out to a radius."},
            {Frost::LightType::Spot, "A cone of light from a point, out to a distance."},
    };

    ImGui::TextDisabled("Drag a light into the viewport, or double-click to add one in front of the camera.");
    ImGui::Spacing();

    for (const Entry& entry : kEntries) {
        ImGui::PushID(static_cast<int>(entry.type));

        const bool activated = ImGui::Selectable(EditorUI::LightDisplayName(entry.type), false,
                                                 ImGuiSelectableFlags_AllowDoubleClick) &&
                               ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        EditorUI::LightDragSource(entry.type);
        ImGui::SetItemTooltip("%s", entry.description);

        // The spawn point is where a drop would be if it hit nothing; lights are offset from the
        // "surface" the same way as when dropped.
        if (activated) { context.SpawnLight(entry.type, context.GetSpawnPoint()); }

        ImGui::SameLine(ImGui::GetFontSize() * 12.0f);
        ImGui::TextDisabled("%s", entry.description);

        ImGui::PopID();
    }
}

void AssetLibraryPanel::OpenImportDialog(EditorContext& context, const std::filesystem::path& source,
                                         const std::optional<std::filesystem::path>& destination) {
    m_OpenImportDialog = true;
    m_ImportError.clear();
    m_FixedDestination = destination;
    m_TextureFolderEdited = false;

    if (!source.empty()) {
        m_ImportSource = Frost::PathToUtf8(source);
        m_ImportBrowser.Navigate(source);
    }
    else if (destination.has_value()) {
        m_ImportSource.clear();
    }
    else if (m_ImportBrowser.GetSelectedFile().has_value()) {
        m_ImportSource = Frost::PathToUtf8(*m_ImportBrowser.GetSelectedFile());
    }

    m_SuggestedForSource = "\x01";  // Force a fresh suggestion.
    SuggestImportDestination(context);
}

void AssetLibraryPanel::SuggestImportDestination(EditorContext& context) {
    if (m_ImportSource == m_SuggestedForSource) { return; }
    m_SuggestedForSource = m_ImportSource;

    AssetLibrary& library = context.GetLibrary();
    const std::filesystem::path content = library.GetContentDirectory();
    const auto relative = [&](const std::filesystem::path& folder) {
        const std::filesystem::path path = folder.lexically_relative(content);
        return path == "." ? std::string() : Frost::GenericPathToUtf8(path);
    };

    std::filesystem::path destination;
    if (m_FixedDestination.has_value()) {
        destination = *m_FixedDestination;
    }
    else {
        const std::filesystem::path source = Frost::PathFromUtf8(m_ImportSource);
        const std::optional<Frost::AssetType> type = Frost::AssetImporter::GetSourceType(source);
        if (!type.has_value()) { return; }

        // Importing the same source again replaces the asset it became last time (wherever it is
        // now); otherwise it goes into the current folder's Meshes/Textures folder.
        if (const AssetLibrary::Entry* const existing = library.FindBySource(source); existing != nullptr) {
            destination = existing->path;
        }
        else {
            destination = DefaultImportFolder(m_CurrentFolder, *type) / source.stem();
            destination += Frost::AssetImporter::GetAssetExtension(*type);
        }
    }

    m_ImportName = Frost::PathToUtf8(destination.stem());
    m_ImportFolder = relative(destination.parent_path());
    if (!m_TextureFolderEdited) { m_ImportTextureFolder = relative(Frost::AssetImporter::GetDefaultTextureFolder(destination)); }
}

void AssetLibraryPanel::DrawImportDialog(EditorContext& context) {
    constexpr const char* kPopupName = "Import Asset";

    if (m_OpenImportDialog) {
        ImGui::OpenPopup(kPopupName);
        m_OpenImportDialog = false;
    }

    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(780.0f, 560.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(kPopupName, nullptr, ImGuiWindowFlags_NoSavedSettings)) { return; }

    AssetLibrary& library = context.GetLibrary();
    const std::filesystem::path content = library.GetContentDirectory();

    if (m_FixedDestination.has_value()) {
        ImGui::TextColored(kWarningColor, "Reimport '%s' from a different source file:",
                           Frost::PathToUtf8(m_FixedDestination->filename()).c_str());
    }
    ImGui::TextUnformatted("Source file: a model (.gltf, .glb) or image (.png, .jpg). Type or paste its full path, or browse:");
    bool submit = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); }
    submit |= EditorUI::InputText("##ImportSource", m_ImportSource, ImGuiInputTextFlags_EnterReturnsTrue);

    const float footer = ImGui::GetFrameHeightWithSpacing() * 5.0f + ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    const FileBrowser::Result browsed = m_ImportBrowser.Draw("ImportBrowser", ImVec2(0.0f, -footer));
    if (browsed.clicked.has_value()) { m_ImportSource = Frost::PathToUtf8(*browsed.clicked); }
    if (browsed.activated.has_value()) { m_ImportSource = Frost::PathToUtf8(*browsed.activated); }

    SuggestImportDestination(context);

    const std::filesystem::path source = Frost::AssetManager::NormalizePath(Frost::PathFromUtf8(m_ImportSource));
    const std::optional<Frost::AssetType> type = Frost::AssetImporter::GetSourceType(source);
    const bool fixed = m_FixedDestination.has_value();
    const float labelWidth = ImGui::CalcTextSize("Its textures").x + ImGui::GetStyle().ItemSpacing.x * 3.0f;

    // Where it goes: name and folder (content-relative), and for a model, where its textures go.
    ImGui::BeginDisabled(fixed);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    submit |= EditorUI::InputText("##ImportName", m_ImportName, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", type.has_value() ? Frost::AssetImporter::GetAssetExtension(*type) : "");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Folder");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (EditorUI::InputText("##ImportFolder", m_ImportFolder) && !m_TextureFolderEdited) {
        std::filesystem::path mesh = content / Frost::PathFromUtf8(m_ImportFolder) / "x.fmesh";
        m_ImportTextureFolder = Frost::GenericPathToUtf8(Frost::AssetImporter::GetDefaultTextureFolder(mesh).lexically_relative(content));
    }
    ImGui::EndDisabled();

    const bool isModel = type == Frost::AssetType::Mesh;
    ImGui::BeginDisabled(!isModel);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Its textures");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (EditorUI::InputText("##ImportTextureFolder", m_ImportTextureFolder)) { m_TextureFolderEdited = true; }
    ImGui::SetItemTooltip("Folder (inside the content directory) for the textures a model comes with; each becomes a\n"
                          "texture asset of its own. Textures from an earlier import of the same model are updated\n"
                          "where they are.");
    ImGui::EndDisabled();

    // Validate and resolve.
    const std::filesystem::path folder = Frost::AssetManager::NormalizePath(content / Frost::PathFromUtf8(m_ImportFolder));
    const std::filesystem::path textureFolder =
            Frost::AssetManager::NormalizePath(content / Frost::PathFromUtf8(m_ImportTextureFolder));

    std::filesystem::path destination;
    std::string problem;
    if (m_ImportSource.empty()) { problem = "Choose a file to import."; }
    else if (!type.has_value()) { problem = "Not a file that can be imported (expected .gltf, .glb, .png, .jpg or .jpeg)."; }
    else if (!FileExists(source)) { problem = "File not found."; }
    else if (fixed) {
        destination = *m_FixedDestination;
        if (Frost::AssetManager::GetAssetType(destination) != type) { problem = "That file is not the same kind of asset."; }
    }
    else if (!IsValidAssetName(m_ImportName)) { problem = "Enter a name for the asset (a plain file name)."; }
    else if (!library.IsInside(folder) || (isModel && !library.IsInside(textureFolder))) {
        problem = "Folders must be inside the content directory.";
    }
    else {
        destination = folder / Frost::PathFromUtf8(m_ImportName);
        destination += Frost::AssetImporter::GetAssetExtension(*type);
        destination = Frost::AssetManager::NormalizePath(destination);
    }

    // Status: what will happen.
    if (!problem.empty()) { ImGui::TextDisabled("%s", problem.c_str()); }
    else {
        ImGui::TextDisabled("-> %s", Frost::GenericPathToUtf8(destination.lexically_relative(content)).c_str());

        const AssetLibrary::Entry* const existing = library.FindByPath(destination);
        if (existing != nullptr && existing->sourcePath == source) {
            ImGui::TextColored(kWarningColor, "Reimport: updates the asset imported earlier from this file (references stay valid).");
        }
        else if (existing != nullptr || FileExists(destination)) {
            ImGui::TextColored(kWarningColor, "Replaces the existing asset '%s' (it keeps its identity, so references stay valid).",
                               Frost::PathToUtf8(destination.filename()).c_str());
        }
        else {
            ImGui::NewLine();
        }
    }

    if (!m_ImportError.empty()) { ImGui::TextColored(kErrorColor, "%s", m_ImportError.c_str()); }
    else { ImGui::NewLine(); }

    ImGui::BeginDisabled(!problem.empty());
    submit |= ImGui::Button("Import", ImVec2(120.0f, 0.0f));
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (submit && problem.empty()) {
        m_ImportError = context.ImportAsset(source, destination, isModel ? textureFolder : std::filesystem::path{});
        if (m_ImportError.empty()) {
            m_Filter.clear();                         // Make sure the new asset is visible...
            m_CurrentFolder = destination.parent_path();  // ...by showing where it went.
            ImGui::CloseCurrentPopup();
        }
    }
    else if (cancel) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

std::string AssetLibraryPanel::DisplayFolder(EditorContext& context, const std::filesystem::path& folder) const {
    const std::filesystem::path relative = folder.lexically_relative(context.GetLibrary().GetContentDirectory());
    return relative.empty() || relative == "." ? "Content" : "Content/" + Frost::GenericPathToUtf8(relative);
}
