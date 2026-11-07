#include "AssetLibraryPanel.hpp"

#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetImporter.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Assets/GltfImporter.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Scene/Level.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

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
        const bool isTexture = type == Frost::AssetType::Texture;
        const std::string wanted = isTexture ? "Textures" : "Meshes";

        if (name == Lowercase(wanted)) { return current; }
        // A sibling kind of folder (Weapons/Glock19/Textures -> Weapons/Glock19/Meshes).
        for (const char* const sibling : {"meshes", "textures", "animations"}) {
            if (name == sibling) { return current.parent_path() / wanted; }
        }
        return current / wanted;
    }

    // A file name made from `text` (characters not allowed in one become '_').
    [[nodiscard]]
    std::string ToAssetName(const std::string& text) {
        std::string name;
        for (const char c : text) {
            const bool invalid = static_cast<unsigned char>(c) < 0x20 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos;
            name.push_back(invalid ? '_' : c);
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '.')) { name.pop_back(); }
        return name.empty() ? "Asset" : name;
    }

    // "1.25 s".
    [[nodiscard]]
    std::string FormatSeconds(const float seconds) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f s", static_cast<double>(seconds));
        return text;
    }

    // Shown for a source item ("texture:Grip" -> "Grip", "animation#2" -> "2").
    [[nodiscard]]
    std::string ItemDisplayName(const std::string& item) {
        const std::size_t separator = item.find_first_of(":#");
        return separator == std::string::npos ? item : item.substr(separator + 1);
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
    // A texture a model came with is updated by reimporting the model; an animation (also one item
    // of its source) is reimported by itself.
    const bool partOfModel = !entry.sourceItem.empty() && entry.type != Frost::AssetType::Animation;
    const bool isModel = entry.type == Frost::AssetType::Mesh || entry.type == Frost::AssetType::SkeletalMesh;

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
        const char* hint = "";
        switch (entry.type) {
            case Frost::AssetType::Mesh: hint = "Drag into the viewport to place it, or onto a folder to move it."; break;
            case Frost::AssetType::SkeletalMesh:
                hint = "Double-click to edit its sockets and skeleton. Drag into the viewport to place it, or onto a folder to move it.";
                break;
            case Frost::AssetType::Texture: hint = "Drag onto a texture slot in the Inspector, or onto a folder to move it."; break;
            case Frost::AssetType::Animation:
                hint = "Drag onto a skeletal mesh's animation slot in the Inspector, or onto a folder to move it.";
                break;
            case Frost::AssetType::Skeleton:
                hint = "The bones its skeletal meshes and animations are bound to. Double-click to edit it; drag onto a folder to move it.";
                break;
        }
        ImGui::SetItemTooltip("%s\n%s", Frost::PathToUtf8(entry.path).c_str(), hint);
    }

    // Double-clicking a skeletal mesh or skeleton opens its editor; a (static) model is placed in
    // front of the camera.
    const bool editable = entry.type == Frost::AssetType::SkeletalMesh || entry.type == Frost::AssetType::Skeleton;
    if (activated && editable) { context.RequestOpenAssetEditor(entry.path); }
    else if (activated && isModel) { context.SpawnModel(entry.path, context.GetSpawnPoint()); }

    if (ImGui::BeginPopupContextItem()) {
        const std::filesystem::path path = entry.path;
        if (editable && ImGui::MenuItem("Edit...")) { context.RequestOpenAssetEditor(path); }
        if (isModel && ImGui::MenuItem("Add to Level")) { context.SpawnModel(path, context.GetSpawnPoint()); }

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
    ImGui::TextUnformatted(EditorUI::AssetTypeDisplayName(entry.type));

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
        if (!entry.sourceItem.empty()) { source += " > " + ItemDisplayName(entry.sourceItem); }

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

            // The open level may have unsaved uses the files do not show: objects whose model,
            // animation, skeleton or a texture override is the asset.
            const Frost::AssetId id = entry->id;
            const auto overridesUse = [id](const auto& mesh) {
                for (std::size_t i = 0; i < mesh.GetMaterialSlotCount(); ++i) {
                    if (const auto& materialOverride = mesh.GetMaterialOverride(i); materialOverride.has_value()) {
                        for (const Frost::TextureOverride& texture : materialOverride->textures) {
                            if (texture.source == Frost::TextureOverride::Source::Asset && texture.texture.id == id) { return true; }
                        }
                    }
                }
                return false;
            };

            for (const std::unique_ptr<Frost::GameObject>& object : context.GetLevel().GetObjects()) {
                bool uses = false;
                if (const Frost::MeshComponent* const mesh = object->TryGetComponent<Frost::MeshComponent>()) {
                    uses = mesh->GetMeshReference().id == id || overridesUse(*mesh);
                }
                if (const Frost::SkeletalMeshComponent* const mesh = object->TryGetComponent<Frost::SkeletalMeshComponent>()) {
                    const bool usesSkeleton = mesh->GetSkeletalMesh() != nullptr && mesh->GetSkeletalMesh()->skeletonReference.id == id;
                    uses = uses || mesh->GetSkeletalMeshReference().id == id || mesh->GetAnimationReference().id == id ||
                           usesSkeleton || overridesUse(*mesh);
                }
                if (const Frost::AnimationTriggerComponent* const triggers = object->TryGetComponent<Frost::AnimationTriggerComponent>()) {
                    for (const Frost::AnimationTrigger& trigger : triggers->triggers) { uses = uses || trigger.animationReference.id == id; }
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
    m_AnimationFolderEdited = false;
    m_FixedAnimation = 0;

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

    const std::filesystem::path source = Frost::AssetManager::NormalizePath(Frost::PathFromUtf8(m_ImportSource));
    const std::optional<Frost::AssetType> type = Frost::AssetImporter::GetSourceType(source);

    std::filesystem::path destination;
    if (m_FixedDestination.has_value()) {
        destination = *m_FixedDestination;

        // Reimporting an animation from another file: the animation of the same name, if it has one.
        if (Frost::AssetManager::GetAssetType(destination) == Frost::AssetType::Animation && Frost::AssetImporter::IsModelSource(source)) {
            InspectModel(source);
            const std::optional<Frost::AssetHeader> header =
                    Frost::AssetFile::ReadHeader(destination, library.GetRegistry().GetProjectDirectory());
            if (m_Inspection.info.has_value() && header.has_value()) {
                const std::string wanted = ItemDisplayName(header->sourceItem);
                for (std::size_t i = 0; i < m_Inspection.info->animations.size(); ++i) {
                    if (m_Inspection.info->animations[i].name == wanted) { m_FixedAnimation = i; }
                }
            }
        }
    }
    else {
        if (!type.has_value()) { return; }

        if (*type == Frost::AssetType::Mesh) {
            // A model: what it can become is chosen in the dialog (see SuggestModelOutputs).
            InspectModel(source);
            SuggestModelOutputs(context, source);
            return;
        }

        // An image: importing the same source again replaces the texture it became last time
        // (wherever it is now); otherwise it goes into the current folder's Textures folder.
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

void AssetLibraryPanel::InspectModel(const std::filesystem::path& source) {
    std::error_code error;
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(source, error);
    const std::uintmax_t size = error ? 0 : std::filesystem::file_size(source, error);
    if (m_Inspection.source == source && m_Inspection.modified == modified && m_Inspection.size == size) { return; }

    const bool sameFile = m_Inspection.source == source;
    m_Inspection = ModelInspection{.source = source, .modified = modified, .size = size, .info = std::nullopt, .error = {}};
    try {
        Frost::GltfModel model = Frost::GltfModel::Open(source);
        m_Inspection.info = model.GetInfo();
    }
    catch (const Frost::ImportError& e) {
        m_Inspection.error = e.what();
    }

    // The file changed under the dialog: its animations may have too.
    if (sameFile) { m_SuggestedForSource = "\x01"; }
}

void AssetLibraryPanel::SuggestModelOutputs(EditorContext& context, const std::filesystem::path& source) {
    AssetLibrary& library = context.GetLibrary();
    const std::filesystem::path content = library.GetContentDirectory();
    const auto relative = [&](const std::filesystem::path& folder) {
        const std::filesystem::path path = folder.lexically_relative(content);
        return path == "." ? std::string() : Frost::GenericPathToUtf8(path);
    };
    const auto existingPath = [&](const Frost::AssetType type, const std::string& item = {}) -> std::optional<std::filesystem::path> {
        const AssetLibrary::Entry* const entry = library.FindBySource(source, type, item);
        return entry != nullptr ? std::optional(entry->path) : std::nullopt;
    };

    const Frost::GltfModelInfo* const info = m_Inspection.info.has_value() ? &*m_Inspection.info : nullptr;
    const std::string stem = Frost::PathToUtf8(source.stem());

    // Whatever an earlier import of this file made is updated where it is now.
    const std::optional<std::filesystem::path> staticMesh = existingPath(Frost::AssetType::Mesh);
    const std::optional<std::filesystem::path> skeletalMesh = existingPath(Frost::AssetType::SkeletalMesh);
    const std::optional<std::filesystem::path> skeleton = existingPath(Frost::AssetType::Skeleton);

    const std::filesystem::path folder = skeletalMesh.has_value() ? skeletalMesh->parent_path()
                                         : staticMesh.has_value() ? staticMesh->parent_path()
                                         : skeleton.has_value()   ? skeleton->parent_path()
                                                                  : DefaultImportFolder(m_CurrentFolder, Frost::AssetType::Mesh);
    m_ImportFolder = relative(folder);
    if (!m_TextureFolderEdited) { m_ImportTextureFolder = relative(Frost::AssetImporter::GetDefaultTextureFolder(folder / "x.fmesh")); }

    // A model with a skeleton or animations is imported as a skeletal mesh by default, anything else
    // as a static mesh (and whatever was imported from it before, again).
    const bool hasGeometry = info == nullptr || info->meshCount > 0;
    const bool animated = info != nullptr && (info->skinned || !info->animations.empty());
    const auto output = [](const bool enabled, const std::optional<std::filesystem::path>& existing, const std::string& name) {
        return ModelOutput{.enabled = enabled,
                           .name = existing.has_value() ? Frost::PathToUtf8(existing->stem()) : name,
                           .existing = existing};
    };
    m_StaticMesh = output(hasGeometry && (staticMesh.has_value() || !animated), staticMesh, stem);
    m_SkeletalMesh = output(hasGeometry && (skeletalMesh.has_value() || animated), skeletalMesh, stem);
    m_Skeleton = output(true, skeleton, stem + "_Skeleton");

    // The parts stay attached (and separate or not) as the earlier import had them.
    m_AttachLooseParts = true;
    m_PartBones.clear();
    m_SeparateParts.clear();
    if (skeletalMesh.has_value()) {
        for (const auto& [part, attachment] :
             Frost::AssetImporter::GetPartAttachments(*skeletalMesh, library.GetRegistry().GetProjectDirectory())
                     .value_or(std::map<std::string, Frost::AssetImporter::PartAttachment>{})) {
            m_PartBones[part] = attachment.bone;
            m_SeparateParts[part] = attachment.separate;
        }
    }
    m_PreviousPartBones = m_PartBones;

    m_Animations.clear();
    std::optional<std::filesystem::path> animationFolder;
    Frost::AssetReference previousSkeleton;  // The one an earlier import of these animations used.
    if (info != nullptr) {
        for (std::size_t i = 0; i < info->animations.size(); ++i) {
            const Frost::GltfAnimationInfo& animation = info->animations[i];
            const std::optional<std::filesystem::path> existing =
                    existingPath(Frost::AssetType::Animation, Frost::AssetImporter::GetAnimationItem(*info, i));
            if (existing.has_value() && !animationFolder.has_value()) {
                animationFolder = existing->parent_path();
                try {
                    previousSkeleton = Frost::AssetFile::ReadAnimation(*existing, library.GetRegistry().GetProjectDirectory()).skeleton;
                }
                catch (const Frost::ImportError&) {
                }
            }
            // Named after the model, unless the animation's own name already is ("AK12_Firing" in
            // AK12.glb stays "AK12_Firing", not "AK12_AK12_Firing").
            const bool prefixed = Lowercase(animation.name).starts_with(Lowercase(stem));
            m_Animations.push_back(output(animation.problem.empty(), existing,
                                          ToAssetName(prefixed ? animation.name : stem + "_" + animation.name)));
        }
    }
    if (!m_AnimationFolderEdited) {
        m_AnimationFolder = relative(animationFolder.value_or(Frost::AssetImporter::GetDefaultAnimationFolder(folder / "x.fskmesh")));
    }

    // Bind to the skeleton the animations were bound to last time if that is not this file's own;
    // a file with animations but nothing to show (an animation pack) to an existing skeleton.
    m_UseExistingSkeleton = false;
    m_ExistingSkeleton = {};
    const AssetLibrary::Entry* const ownSkeleton = library.FindBySource(source, Frost::AssetType::Skeleton);
    if (previousSkeleton.IsSet() && (ownSkeleton == nullptr || ownSkeleton->id != previousSkeleton.id)) {
        m_UseExistingSkeleton = true;
        m_ExistingSkeleton = previousSkeleton;
    }
    else if (!hasGeometry && !skeleton.has_value()) {
        for (const AssetLibrary::Entry& entry : library.GetEntries()) {
            if (entry.type == Frost::AssetType::Skeleton && !entry.duplicate) {
                m_UseExistingSkeleton = true;
                m_ExistingSkeleton = Frost::AssetReference{.id = entry.id, .path = entry.path};
                break;
            }
        }
    }
}

void AssetLibraryPanel::DrawImportDialog(EditorContext& context) {
    constexpr const char* kPopupName = "Import Asset";

    if (m_OpenImportDialog) {
        ImGui::OpenPopup(kPopupName);
        m_OpenImportDialog = false;
    }

    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820.0f, 680.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(kPopupName, nullptr, ImGuiWindowFlags_NoSavedSettings)) { return; }

    if (m_FixedDestination.has_value()) {
        ImGui::TextColored(kWarningColor, "Reimport '%s' from a different source file:",
                           Frost::PathToUtf8(m_FixedDestination->filename()).c_str());
    }
    ImGui::TextUnformatted("Source file: a model (.gltf, .glb) or image (.png, .jpg). Type or paste its full path, or browse:");
    bool submit = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); }
    submit |= EditorUI::InputText("##ImportSource", m_ImportSource, ImGuiInputTextFlags_EnterReturnsTrue);

    const float browserHeight = std::max(ImGui::GetTextLineHeightWithSpacing() * 6.0f, ImGui::GetContentRegionAvail().y * 0.3f);
    const FileBrowser::Result browsed = m_ImportBrowser.Draw("ImportBrowser", ImVec2(0.0f, browserHeight));
    if (browsed.clicked.has_value()) { m_ImportSource = Frost::PathToUtf8(*browsed.clicked); }
    if (browsed.activated.has_value()) { m_ImportSource = Frost::PathToUtf8(*browsed.activated); }

    const std::filesystem::path source = Frost::AssetManager::NormalizePath(Frost::PathFromUtf8(m_ImportSource));
    if (Frost::AssetImporter::IsModelSource(source) && FileExists(source)) { InspectModel(source); }
    SuggestImportDestination(context);

    // What to import it as, and where (scrolls if a model has many animations).
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f +
                         ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImportPlan plan;
    if (ImGui::BeginChild("##ImportOptions", ImVec2(0.0f, -footer), ImGuiChildFlags_None)) {
        const bool modelOptions = !m_FixedDestination.has_value() && Frost::AssetImporter::IsModelSource(source) &&
                                  FileExists(source);
        plan = modelOptions ? DrawModelImportOptions(context, source, submit) : DrawSingleImportOptions(context, source, submit);
    }
    ImGui::EndChild();

    if (!plan.problem.empty()) { ImGui::TextDisabled("%s", plan.problem.c_str()); }
    else { ImGui::NewLine(); }
    if (!m_ImportError.empty()) { ImGui::TextColored(kErrorColor, "%s", m_ImportError.c_str()); }
    else { ImGui::NewLine(); }

    const bool ready = plan.problem.empty() && plan.run;
    ImGui::BeginDisabled(!ready);
    submit |= ImGui::Button("Import", ImVec2(120.0f, 0.0f));
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (submit && ready) {
        m_ImportError = plan.run();
        if (m_ImportError.empty()) {
            m_Filter.clear();  // Make sure the new assets are visible...
            if (!plan.folder.empty()) { m_CurrentFolder = plan.folder; }  // ...by showing where they went.
            ImGui::CloseCurrentPopup();
        }
    }
    else if (cancel) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

namespace {

    // What importing to `destination` does to what is there, shown after an output's name.
    void DrawDestinationStatus(const std::filesystem::path& destination, const std::optional<std::filesystem::path>& existing) {
        ImGui::SameLine();
        if (existing.has_value() && *existing == destination) {
            ImGui::TextColored(kWarningColor, "updates it");
            ImGui::SetItemTooltip("Reimport: updates %s, imported earlier from this file (its id is kept, so references stay valid).",
                                  Frost::PathToUtf8(destination).c_str());
        }
        else if (FileExists(destination)) {
            ImGui::TextColored(kWarningColor, "replaces it");
            ImGui::SetItemTooltip("Replaces the existing asset %s (it keeps its identity, so references stay valid).",
                                  Frost::PathToUtf8(destination).c_str());
        }
        else {
            ImGui::TextDisabled("new");
            ImGui::SetItemTooltip("%s", Frost::PathToUtf8(destination).c_str());
        }
    }

}  // namespace

AssetLibraryPanel::ImportPlan AssetLibraryPanel::DrawModelImportOptions(EditorContext& context, const std::filesystem::path& source,
                                                                       bool& submit) {
    AssetLibrary& library = context.GetLibrary();
    const std::filesystem::path content = library.GetContentDirectory();

    if (!m_Inspection.error.empty() || !m_Inspection.info.has_value()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", m_Inspection.error.empty() ? "The model could not be read." : m_Inspection.error.c_str());
        ImGui::PopStyleColor();
        return ImportPlan{.problem = "The model cannot be imported.", .run = {}, .folder = {}};
    }
    const Frost::GltfModelInfo& info = *m_Inspection.info;
    const bool hasGeometry = info.meshCount > 0;

    // What is in it.
    std::string summary = std::to_string(info.meshCount) + (info.meshCount == 1 ? " mesh" : " meshes");
    if (info.skeletonProblem.empty()) {
        summary += ", a skeleton of " + std::to_string(info.boneCount) + " bones";
        summary += info.skinned ? " (from " + std::to_string(info.skinCount) + (info.skinCount == 1 ? " skin)" : " skins)")
                                : " (its node hierarchy)";
    }
    summary += ", " + std::to_string(info.animations.size()) + (info.animations.size() == 1 ? " animation" : " animations");
    ImGui::TextDisabled("Contains %s.", summary.c_str());
    if (!info.skeletonProblem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("Its skeleton cannot be made: %s", info.skeletonProblem.c_str());
        ImGui::PopStyleColor();
    }

    const float labelWidth = ImGui::CalcTextSize("Skeletal mesh").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 3.0f;
    const float nameWidth = ImGui::GetFontSize() * 16.0f;
    const auto relativeToContent = [&](const std::filesystem::path& path) {
        return Frost::GenericPathToUtf8(path.lexically_relative(content));
    };

    // Where it goes.
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Folder");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (EditorUI::InputText("##ImportFolder", m_ImportFolder)) {
        const std::filesystem::path mesh = content / Frost::PathFromUtf8(m_ImportFolder) / "x.fmesh";
        if (!m_TextureFolderEdited) { m_ImportTextureFolder = relativeToContent(Frost::AssetImporter::GetDefaultTextureFolder(mesh)); }
        if (!m_AnimationFolderEdited) { m_AnimationFolder = relativeToContent(Frost::AssetImporter::GetDefaultAnimationFolder(mesh)); }
    }
    ImGui::SetItemTooltip("Folder (inside the content directory) for the meshes and the skeleton.");

    const bool anyMesh = m_StaticMesh.enabled || m_SkeletalMesh.enabled;
    ImGui::BeginDisabled(!anyMesh);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Its textures");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (EditorUI::InputText("##ImportTextureFolder", m_ImportTextureFolder)) { m_TextureFolderEdited = true; }
    ImGui::SetItemTooltip("Folder for the textures the model comes with; each becomes a texture asset of its own.\n"
                          "Textures from an earlier import of the same model are updated where they are.");
    ImGui::EndDisabled();

    const std::filesystem::path folder = Frost::AssetManager::NormalizePath(content / Frost::PathFromUtf8(m_ImportFolder));
    const std::filesystem::path textureFolder =
            Frost::AssetManager::NormalizePath(content / Frost::PathFromUtf8(m_ImportTextureFolder));
    const std::filesystem::path animationFolder =
            Frost::AssetManager::NormalizePath(content / Frost::PathFromUtf8(m_AnimationFolder));

    // An output's file: the asset an earlier import made while it keeps that asset's name, else a
    // new one in its folder.
    const auto destinationOf = [](const ModelOutput& output, const std::filesystem::path& outputFolder, const Frost::AssetType type) {
        if (output.existing.has_value() && Frost::PathToUtf8(output.existing->stem()) == output.name) { return *output.existing; }
        std::filesystem::path path = outputFolder / Frost::PathFromUtf8(output.name);
        path += Frost::AssetImporter::GetAssetExtension(type);
        return Frost::AssetManager::NormalizePath(path);
    };

    // One row: a checkbox, the name (and extension), and what importing it does.
    const auto outputRow = [&](const char* label, const char* id, ModelOutput& output, const Frost::AssetType type,
                               const std::filesystem::path& outputFolder, const bool available, const char* tooltip) {
        ImGui::PushID(id);
        ImGui::BeginDisabled(!available);
        if (!available) { output.enabled = false; }
        ImGui::Checkbox(label, &output.enabled);
        ImGui::SetItemTooltip("%s", tooltip);
        ImGui::SameLine(labelWidth);
        ImGui::BeginDisabled(!output.enabled);
        ImGui::SetNextItemWidth(nameWidth);
        submit |= EditorUI::InputText("##Name", output.name, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Frost::AssetImporter::GetAssetExtension(type));
        if (output.enabled && IsValidAssetName(output.name)) { DrawDestinationStatus(destinationOf(output, outputFolder, type), output.existing); }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    };

    ImGui::SeparatorText("Import as");
    outputRow("Static mesh", "Static", m_StaticMesh, Frost::AssetType::Mesh, folder, hasGeometry,
              "The model as posed in the file, as one rigid mesh (props, level geometry).");
    outputRow("Skeletal mesh", "Skeletal", m_SkeletalMesh, Frost::AssetType::SkeletalMesh, folder, hasGeometry,
              "The model bound to a skeleton, which animations pose (characters, animated weapons).");

    // The skeletal mesh's rigid parts (meshes that are not skinned): whether each is part of the
    // skeletal mesh or an object of its own attached to it, and the bone each follows.
    if (!info.parts.empty()) {
        ImGui::BeginDisabled(!m_SkeletalMesh.enabled);
        ImGui::SetCursorPosX(labelWidth);
        const std::string header = "Attached parts (" + std::to_string(info.parts.size()) + ")###AttachedParts";
        if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Attach loose parts to bones", &m_AttachLooseParts);
            ImGui::SetItemTooltip("Parts that are not under a bone in the file (e.g. a magazine next to the armature rather\n"
                                  "than parented to a bone) follow the bone suggested for them (one whose name matches the\n"
                                  "part's, else the skeleton's root). Off: they do not move with the skeleton.");
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("A separate part is imported as a model of its own, placed as an object attached to the skeletal "
                               "mesh, so a level can remove it or swap it (a suppressor, an extended magazine).");
            ImGui::PopStyleColor();
            const float modeWidth = ImGui::CalcTextSize("Separate object").x + ImGui::GetFrameHeight() +
                                    ImGui::GetStyle().FramePadding.x * 2.0f;
            float partNameWidth = ImGui::CalcTextSize("(unnamed)").x;
            for (const Frost::GltfPartInfo& part : info.parts) { partNameWidth = std::max(partNameWidth, ImGui::CalcTextSize(part.name.c_str()).x); }
            const float modeColumn = ImGui::GetCursorPosX() + partNameWidth + ImGui::GetStyle().ItemSpacing.x * 2.0f;

            for (const Frost::GltfPartInfo& part : info.parts) {
                ImGui::PushID(part.name.c_str());

                // Separate or not: as chosen (or imported before), else loose parts are separate.
                const auto separateChoice = m_SeparateParts.find(part.name);
                const bool separate = separateChoice != m_SeparateParts.end() ? separateChoice->second : part.parentBone.empty();

                // What the importer will do for the part: the choice made here, else the file's
                // hierarchy, else the suggestion (if loose parts are attached).
                const auto chosen = m_PartBones.find(part.name);
                const std::string bone = chosen != m_PartBones.end() ? chosen->second
                                         : !part.parentBone.empty() ? part.parentBone
                                         : m_AttachLooseParts       ? part.suggestedBone
                                                                    : std::string();
                const auto previous = m_PreviousPartBones.find(part.name);
                const bool asBefore = chosen != m_PartBones.end() && previous != m_PreviousPartBones.end() &&
                                      previous->second == chosen->second;
                const char* why = asBefore                      ? "as imported before"
                                  : chosen != m_PartBones.end() ? "chosen"
                                  : !part.parentBone.empty()    ? "under it in the file"
                                  : m_AttachLooseParts          ? "suggested"
                                                                : "";

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(part.name.empty() ? "(unnamed)" : part.name.c_str());
                ImGui::SameLine(modeColumn);
                ImGui::SetNextItemWidth(modeWidth);
                if (ImGui::BeginCombo("##Mode", separate ? "Separate object" : "Part of mesh")) {
                    if (ImGui::Selectable("Part of mesh", !separate)) { m_SeparateParts[part.name] = false; }
                    ImGui::SetItemTooltip("Baked into the skeletal mesh, following its bone.");
                    if (ImGui::Selectable("Separate object", separate)) { m_SeparateParts[part.name] = true; }
                    ImGui::SetItemTooltip("A model of its own (<skeletal mesh>_%s), attached to the skeletal mesh's object\n"
                                          "when it is placed, following its bone; removable and replaceable.",
                                          part.name.c_str());
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                const char* const noBone = separate ? "(the object itself)" : "(does not follow a bone)";
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
                if (ImGui::BeginCombo("##Bone", bone.empty() ? noBone : bone.c_str())) {
                    if (ImGui::Selectable(noBone, bone.empty())) { m_PartBones[part.name] = std::string(); }
                    for (const std::string& name : info.boneNames) {
                        if (ImGui::Selectable(name.c_str(), name == bone)) { m_PartBones[part.name] = name; }
                    }
                    ImGui::EndCombo();
                }
                if (*why != '\0') {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", why);
                }
                if (chosen != m_PartBones.end() || separateChoice != m_SeparateParts.end()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Default")) {
                        m_PartBones.erase(part.name);
                        m_SeparateParts.erase(part.name);
                    }
                    ImGui::SetItemTooltip("Back to what the file and the suggestion say (loose parts separate).");
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
    }

    // The skeleton the skeletal mesh and animations are bound to.
    const bool anyAnimation = std::any_of(m_Animations.begin(), m_Animations.end(), [](const ModelOutput& output) { return output.enabled; });
    const bool needsSkeleton = m_SkeletalMesh.enabled || anyAnimation;
    const bool ownSkeletonAvailable = info.skeletonProblem.empty();
    if (!ownSkeletonAvailable) { m_UseExistingSkeleton = true; }

    ImGui::BeginDisabled(!needsSkeleton);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Skeleton");
    ImGui::SameLine(labelWidth);
    ImGui::BeginDisabled(!ownSkeletonAvailable);
    if (ImGui::RadioButton("The model's##OwnSkeleton", !m_UseExistingSkeleton)) { m_UseExistingSkeleton = false; }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Import the model's own skeleton (and bind the skeletal mesh and animations to it).");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_UseExistingSkeleton);
    ImGui::SetNextItemWidth(nameWidth - ImGui::CalcTextSize("The model's").x - ImGui::GetFrameHeight() -
                            ImGui::GetStyle().ItemSpacing.x * 2.0f);
    submit |= EditorUI::InputText("##SkeletonName", m_Skeleton.name, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Frost::AssetImporter::GetAssetExtension(Frost::AssetType::Skeleton));
    if (!m_UseExistingSkeleton && needsSkeleton && IsValidAssetName(m_Skeleton.name)) {
        DrawDestinationStatus(destinationOf(m_Skeleton, folder, Frost::AssetType::Skeleton), m_Skeleton.existing);
    }
    ImGui::EndDisabled();

    // Or one already in the project (e.g. the character's, for an animation pack of the same rig).
    const std::optional<std::filesystem::path> existingSkeletonPath =
            m_ExistingSkeleton.IsSet() ? library.GetRegistry().Resolve(m_ExistingSkeleton) : std::nullopt;
    ImGui::SetCursorPosX(labelWidth);
    if (ImGui::RadioButton("Existing##ExistingSkeleton", m_UseExistingSkeleton)) { m_UseExistingSkeleton = true; }
    ImGui::SetItemTooltip("Bind the skeletal mesh and animations to a skeleton already in the project, by bone name\n"
                          "(e.g. a character's, when importing more animations of the same rig from another file).");
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_UseExistingSkeleton);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string skeletonLabel = existingSkeletonPath.has_value() ? relativeToContent(*existingSkeletonPath) : "(choose a skeleton)";
    if (ImGui::BeginCombo("##ExistingSkeletonCombo", skeletonLabel.c_str())) {
        bool any = false;
        for (const AssetLibrary::Entry& entry : library.GetEntries()) {
            if (entry.type != Frost::AssetType::Skeleton || entry.duplicate) { continue; }
            any = true;
            if (ImGui::Selectable(relativeToContent(entry.path).c_str(), entry.id == m_ExistingSkeleton.id)) {
                m_ExistingSkeleton = Frost::AssetReference{.id = entry.id, .path = entry.path};
            }
        }
        if (!any) { ImGui::TextDisabled("No skeletons in the project yet."); }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // Animations, one asset each.
    ImGui::SeparatorText(("Animations (" + std::to_string(info.animations.size()) + ")").c_str());
    if (info.animations.empty()) { ImGui::TextDisabled("The model has no animations."); }
    else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Folder");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7.0f);
        if (EditorUI::InputText("##AnimationFolder", m_AnimationFolder)) { m_AnimationFolderEdited = true; }
        ImGui::SetItemTooltip("Folder for new animation assets (animations imported earlier are updated where they are).");
        ImGui::SameLine();
        if (ImGui::SmallButton("All")) {
            for (std::size_t i = 0; i < m_Animations.size(); ++i) { m_Animations[i].enabled = info.animations[i].problem.empty(); }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) {
            for (ModelOutput& animation : m_Animations) { animation.enabled = false; }
        }

        for (std::size_t i = 0; i < m_Animations.size() && i < info.animations.size(); ++i) {
            const Frost::GltfAnimationInfo& animation = info.animations[i];
            ModelOutput& output = m_Animations[i];
            const std::string label = animation.name + "  (" + FormatSeconds(animation.duration) + ")";

            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(!animation.problem.empty());
            if (!animation.problem.empty()) { output.enabled = false; }
            ImGui::Checkbox("##Enabled", &output.enabled);
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDisabled();
            if (!animation.problem.empty()) {
                ImGui::SetItemTooltip("Cannot be imported: %s", animation.problem.c_str());
            }
            else {
                ImGui::SetItemTooltip("%zu channel(s).", animation.channelCount);
            }
            ImGui::SameLine(labelWidth + ImGui::GetFontSize() * 9.0f);
            ImGui::BeginDisabled(!output.enabled);
            ImGui::SetNextItemWidth(nameWidth);
            submit |= EditorUI::InputText("##Name", output.name, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Frost::AssetImporter::GetAssetExtension(Frost::AssetType::Animation));
            if (output.enabled && IsValidAssetName(output.name)) {
                DrawDestinationStatus(destinationOf(output, animationFolder, Frost::AssetType::Animation), output.existing);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }

    // Validate, and gather what to do.
    ImportPlan plan;
    Frost::AssetImporter::ModelOptions options;
    std::vector<std::filesystem::path> destinations;
    const auto fail = [&plan](std::string problem) {
        plan.problem = std::move(problem);
        return plan;
    };

    if (!m_StaticMesh.enabled && !m_SkeletalMesh.enabled && !anyAnimation) { return fail("Choose what to import the model as."); }
    if (!library.IsInside(folder) || (anyMesh && !library.IsInside(textureFolder)) || (anyAnimation && !library.IsInside(animationFolder))) {
        return fail("Folders must be inside the content directory.");
    }

    if (m_StaticMesh.enabled) {
        if (!IsValidAssetName(m_StaticMesh.name)) { return fail("Enter a name for the static mesh (a plain file name)."); }
        options.mesh = destinationOf(m_StaticMesh, folder, Frost::AssetType::Mesh);
        destinations.push_back(options.mesh);
    }
    if (m_SkeletalMesh.enabled) {
        if (!IsValidAssetName(m_SkeletalMesh.name)) { return fail("Enter a name for the skeletal mesh (a plain file name)."); }
        options.skeletalMesh = destinationOf(m_SkeletalMesh, folder, Frost::AssetType::SkeletalMesh);
        destinations.push_back(options.skeletalMesh);
    }
    if (needsSkeleton) {
        if (m_UseExistingSkeleton) {
            if (!existingSkeletonPath.has_value()) { return fail("Choose the skeleton to bind the skeletal mesh and animations to."); }
            options.existingSkeleton = Frost::AssetReference{.id = m_ExistingSkeleton.id, .path = *existingSkeletonPath};
        }
        else {
            if (!IsValidAssetName(m_Skeleton.name)) { return fail("Enter a name for the skeleton (a plain file name)."); }
            options.skeleton = destinationOf(m_Skeleton, folder, Frost::AssetType::Skeleton);
            destinations.push_back(options.skeleton);
        }
    }
    for (std::size_t i = 0; i < m_Animations.size(); ++i) {
        if (!m_Animations[i].enabled) { continue; }
        if (!IsValidAssetName(m_Animations[i].name)) {
            return fail("Enter a name for animation '" + info.animations[i].name + "' (a plain file name).");
        }
        options.animations.push_back(Frost::AssetImporter::ModelOptions::Animation{
                .index = i, .destination = destinationOf(m_Animations[i], animationFolder, Frost::AssetType::Animation)});
        destinations.push_back(options.animations.back().destination);
    }

    std::sort(destinations.begin(), destinations.end());
    if (std::adjacent_find(destinations.begin(), destinations.end()) != destinations.end()) {
        return fail("Two of the assets would have the same file; give them different names.");
    }

    options.textureFolder = textureFolder;
    options.attachLooseParts = m_AttachLooseParts;
    options.partBones = m_PartBones;
    for (const Frost::GltfPartInfo& part : info.parts) {
        const auto choice = m_SeparateParts.find(part.name);
        if (choice != m_SeparateParts.end() ? choice->second : part.parentBone.empty()) { options.separateParts.insert(part.name); }
    }
    plan.folder = !options.skeletalMesh.empty() ? options.skeletalMesh.parent_path()
                  : !options.mesh.empty()       ? options.mesh.parent_path()
                                                : options.animations.front().destination.parent_path();
    plan.run = [&context, source, options] { return context.ImportModel(source, options); };
    return plan;
}

AssetLibraryPanel::ImportPlan AssetLibraryPanel::DrawSingleImportOptions(EditorContext& context, const std::filesystem::path& source,
                                                                        bool& submit) {
    AssetLibrary& library = context.GetLibrary();
    const std::filesystem::path content = library.GetContentDirectory();

    const std::optional<Frost::AssetType> sourceType = Frost::AssetImporter::GetSourceType(source);
    const bool fixed = m_FixedDestination.has_value();
    const std::optional<Frost::AssetType> type = fixed ? Frost::AssetManager::GetAssetType(*m_FixedDestination) : sourceType;
    const float labelWidth = ImGui::CalcTextSize("Its textures").x + ImGui::GetStyle().ItemSpacing.x * 3.0f;

    // Where it goes: name and folder (content-relative), and for a mesh, where its textures go.
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

    const bool isMesh = type == Frost::AssetType::Mesh || type == Frost::AssetType::SkeletalMesh;
    ImGui::BeginDisabled(!isMesh);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Its textures");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (EditorUI::InputText("##ImportTextureFolder", m_ImportTextureFolder)) { m_TextureFolderEdited = true; }
    ImGui::SetItemTooltip("Folder (inside the content directory) for the textures a model comes with; each becomes a\n"
                          "texture asset of its own. Textures from an earlier import of the same model are updated\n"
                          "where they are.");
    ImGui::EndDisabled();

    // Reimporting an animation from another file: which of its animations.
    const bool isAnimation = fixed && type == Frost::AssetType::Animation;
    const bool haveAnimations = m_Inspection.source == source && m_Inspection.info.has_value() && !m_Inspection.info->animations.empty();
    if (isAnimation && haveAnimations) {
        const std::vector<Frost::GltfAnimationInfo>& animations = m_Inspection.info->animations;
        m_FixedAnimation = std::min(m_FixedAnimation, animations.size() - 1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Animation");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        if (ImGui::BeginCombo("##FixedAnimation", animations[m_FixedAnimation].name.c_str())) {
            for (std::size_t i = 0; i < animations.size(); ++i) {
                ImGui::BeginDisabled(!animations[i].problem.empty());
                if (ImGui::Selectable((animations[i].name + "  (" + FormatSeconds(animations[i].duration) + ")").c_str(),
                                      i == m_FixedAnimation)) {
                    m_FixedAnimation = i;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
    }
    if (fixed && (type == Frost::AssetType::SkeletalMesh || type == Frost::AssetType::Animation)) {
        ImGui::TextDisabled("It stays bound to the skeleton it was imported for.");
    }

    // Validate and resolve.
    const std::filesystem::path folder = Frost::AssetManager::NormalizePath(content / Frost::PathFromUtf8(m_ImportFolder));
    const std::filesystem::path textureFolder =
            Frost::AssetManager::NormalizePath(content / Frost::PathFromUtf8(m_ImportTextureFolder));

    ImportPlan plan;
    std::filesystem::path destination;
    if (m_ImportSource.empty()) { plan.problem = "Choose a file to import."; }
    else if (!sourceType.has_value()) { plan.problem = "Not a file that can be imported (expected .gltf, .glb, .png, .jpg or .jpeg)."; }
    else if (!FileExists(source)) { plan.problem = "File not found."; }
    else if (fixed) {
        destination = *m_FixedDestination;
        if (!type.has_value() || !Frost::AssetImporter::CanImportAs(source, *type)) {
            plan.problem = "That file cannot be imported as this kind of asset.";
        }
        else if (isAnimation && !m_Inspection.error.empty()) { plan.problem = m_Inspection.error; }
        else if (isAnimation && !haveAnimations) { plan.problem = "That model has no animations."; }
    }
    else if (!IsValidAssetName(m_ImportName)) { plan.problem = "Enter a name for the asset (a plain file name)."; }
    else if (!library.IsInside(folder) || (isMesh && !library.IsInside(textureFolder))) {
        plan.problem = "Folders must be inside the content directory.";
    }
    else {
        destination = folder / Frost::PathFromUtf8(m_ImportName);
        destination += Frost::AssetImporter::GetAssetExtension(*type);
        destination = Frost::AssetManager::NormalizePath(destination);
    }

    // Status: what will happen.
    if (plan.problem.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("-> %s", Frost::GenericPathToUtf8(destination.lexically_relative(content)).c_str());

        const AssetLibrary::Entry* const existing = library.FindByPath(destination);
        if (existing != nullptr && existing->sourcePath == source) {
            ImGui::TextColored(kWarningColor, "Reimport: updates the asset imported earlier from this file (references stay valid).");
        }
        else if (existing != nullptr || FileExists(destination)) {
            ImGui::TextColored(kWarningColor, "Replaces the existing asset '%s' (it keeps its identity, so references stay valid).",
                               Frost::PathToUtf8(destination.filename()).c_str());
        }

        Frost::AssetImporter::Options options;
        if (isAnimation) { options.sourceItem = Frost::AssetImporter::GetAnimationItem(*m_Inspection.info, m_FixedAnimation); }
        plan.folder = destination.parent_path();
        plan.run = [&context, source, destination, textureFolder = isMesh ? textureFolder : std::filesystem::path{}, options] {
            return context.ImportAsset(source, destination, textureFolder, options);
        };
    }
    return plan;
}

std::string AssetLibraryPanel::DisplayFolder(EditorContext& context, const std::filesystem::path& folder) const {
    const std::filesystem::path relative = folder.lexically_relative(context.GetLibrary().GetContentDirectory());
    return relative.empty() || relative == "." ? "Content" : "Content/" + Frost::GenericPathToUtf8(relative);
}
