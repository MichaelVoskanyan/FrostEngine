#include "EditorApplication.hpp"

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Engine.hpp>
#include <Frost/Physics/Collision.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Platform/Window.hpp>
#include <Frost/Renderer/Renderer.hpp>
#include <Frost/Scene/LevelSerializer.hpp>
#include <Frost/Units.hpp>

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API, for the default layout.

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

    constexpr const char* kUnsavedPopup = "Unsaved Changes";
    constexpr const char* kMessagesPopup = "Editor Messages";

    // Objects added without a drop location appear this far in front of the camera.
    constexpr float kSpawnDistance = 5.0f * Frost::kMeter;

    // How often the content directory is rescanned for changes made outside the editor.
    constexpr std::chrono::milliseconds kContentPollInterval{1000};

    // What to frame for an object: its model, else its collider (e.g. an invisible ground box, or a
    // character's capsule).
    [[nodiscard]]
    std::optional<Frost::Bounds> GetFramingBounds(const Frost::GameObject& object) {
        if (std::optional<Frost::Bounds> bounds = object.GetWorldBounds(); bounds.has_value()) { return bounds; }

        // Where it is in the world (an attached object moves with what it is attached to).
        Frost::TransformComponent transform;
        transform.SetFromMatrix(object.GetWorldMatrix());
        if (const Frost::BoxColliderComponent* const collider = object.TryGetComponent<Frost::BoxColliderComponent>()) {
            return Frost::MakeWorldBox(transform, *collider).GetBounds();
        }
        if (const auto* const capsule = object.TryGetComponent<Frost::CapsuleColliderComponent>()) {
            return Frost::MakeWorldCapsule(transform, *capsule).GetBounds();
        }
        return std::nullopt;
    }

    // The engine setup for editing `project`; makes sure the project's directories exist first.
    [[nodiscard]]
    Frost::EngineSpecification EditorSpecification(const Frost::Project& project) {
        project.CreateDirectories();
        return Frost::EngineSpecification{.applicationName = "Frost Editor",
                                          .windowWidth = 1600,
                                          .windowHeight = 900,
                                          .windowResizable = true,
                                          .enableImGui = true,
                                          .project = project};
    }

}  // namespace

EditorApplication::EditorApplication(const Frost::Project& project)
    : Frost::Application(EditorSpecification(project)),
      m_Library(GetEngine().GetAssetManager(), GetEngine().GetProject()),
      m_Context(GetEngine(), m_Library),
      // The file system browser starts at the project root, where the source files to import
      // usually are (assets/).
      m_AssetLibraryPanel(GetEngine().GetProject().GetRootDirectory()),
      m_OpenLevelDialog("Open Level", FileDialog::Mode::Open, Frost::Project::kLevelExtension,
                        GetEngine().GetProject().GetLevelsDirectory()),
      m_SaveLevelDialog("Save Level As", FileDialog::Mode::Save, Frost::Project::kLevelExtension,
                        GetEngine().GetProject().GetLevelsDirectory()) {
    m_OpenLevelDialog.SetValidator([this](const std::filesystem::path& path) { return CheckLevelPath(path, false); });
    m_SaveLevelDialog.SetValidator([this](const std::filesystem::path& path) { return CheckLevelPath(path, true); });

    std::cout << "[Editor] Project '" << GetProject().GetName() << "' ("
              << Frost::PathToUtf8(GetProject().GetRootDirectory()) << ")\n";

    // Assets from before asset ids are brought up to date once, up front.
    for (const std::string& message : m_Library.UpgradeLegacyAssets()) { m_Context.ReportError(message); }

    UpdateWindowTitle();
}

void EditorApplication::OnUpdate(const float deltaTime) {
    Frost::Engine& engine = GetEngine();

    // Changes made to the content directory outside the editor (files moved, renamed, added or
    // deleted in the OS file manager): rescan, and let the level pick up anything it was missing.
    if (m_Library.Poll(kContentPollInterval)) {
        for (const std::string& message : m_Library.UpgradeLegacyAssets()) { m_Context.ReportError(message); }
        m_Context.OnAssetsChanged();
    }

    // Closing the window with unsaved changes asks first (the prompt may re-request the close):
    // the asset editors' changes (each editor asks), then the level's.
    if (engine.GetWindow().ShouldClose()) {
        for (const std::unique_ptr<SkeletalAssetEditor>& editor : m_AssetEditors) {
            if (editor->IsDirty()) {
                engine.GetWindow().SetShouldClose(false);
                editor->RequestClose();
                break;
            }
        }
    }
    if (engine.GetWindow().ShouldClose() && m_Context.IsLevelDirty()) {
        engine.GetWindow().SetShouldClose(false);
        RequestAction(PendingAction::Exit);
    }

    // The camera may take the mouse when the right button goes down over the scene (hover state is
    // from the previous UI frame, which is where the cursor was when the button went down).
    const bool wasControlling = m_EditorCamera.IsControlling();
    m_EditorCamera.OnUpdate(deltaTime, engine.GetInput(), m_ViewportPanel.IsHovered());
    if (m_EditorCamera.IsControlling() != wasControlling) { SetUiInputEnabled(!m_EditorCamera.IsControlling()); }

    m_Context.UpdateSimulation(deltaTime);
    m_Context.UpdateAnimationPreviews(deltaTime);
    m_Context.UpdatePlaying();

    if (const std::optional<Frost::ObjectId> focus = m_Context.ConsumeFocusRequest()) { FocusObject(*focus); }
    if (const std::optional<std::filesystem::path> asset = m_Context.ConsumeOpenAssetEditorRequest()) { OpenAssetEditor(*asset); }
    if (std::optional<std::filesystem::path> level = m_Context.ConsumeOpenLevelRequest()) {
        m_PendingLevelFile = std::move(*level);
        RequestAction(PendingAction::OpenLevelFile);
    }

    const Frost::Camera& camera = m_EditorCamera.GetCamera();
    m_Context.SetSpawnPoint(camera.GetPosition() + camera.GetForward() * kSpawnDistance);
    m_Context.SetView(Frost::ViewPoint{.position = m_EditorCamera.GetPosition(),
                                       .yawDegrees = m_EditorCamera.GetYawDegrees(),
                                       .pitchDegrees = m_EditorCamera.GetPitchDegrees()});
    engine.GetRenderer().SetCamera(camera);
}

void EditorApplication::OnImGuiRender() {
    DrawDockSpace();
    DrawMenuBar();

    m_ViewportPanel.Draw(m_Context, m_EditorCamera);
    m_LevelPanel.Draw(m_Context);
    m_InspectorPanel.Draw(m_Context);
    m_AssetLibraryPanel.Draw(m_Context);
    std::erase_if(m_AssetEditors, [this](const std::unique_ptr<SkeletalAssetEditor>& editor) { return !editor->Draw(m_Context); });

    DrawDialogs();
    HandleShortcuts();
    UpdateWindowTitle();

    // Last, so this frame's edits (moves, deletions, new objects) are what gets drawn.
    m_Context.GetLevel().Submit(GetEngine().GetRenderer(), GetEngine().GetAssetManager());
}

void EditorApplication::DrawDockSpace() {
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    const ImGuiID dockSpaceId = ImGui::GetID("EditorDockSpace");

    // First run (no saved layout in imgui.ini) or Window > Reset Layout.
    if (m_ResetLayout || ImGui::DockBuilderGetNode(dockSpaceId) == nullptr) {
        BuildDefaultLayout(dockSpaceId);
        m_ResetLayout = false;
    }

    // The dock space's host window, covering the main window below the menu bar. This is what
    // DockSpaceOverViewport() does, except that the host has no background: the scene is drawn
    // behind the whole UI, and the Viewport window docked in the middle is transparent, so nothing
    // between them may paint over it. (DockSpaceOverViewport's PassthruCentralNode only leaves the
    // central node unpainted while it is empty, which it never is here.) Every other panel paints
    // its own background.
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags kHostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
                                            ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##EditorDockHost", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);

    ImGui::DockSpace(dockSpaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void EditorApplication::BuildDefaultLayout(const ImGuiID dockSpaceId) {
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();

    ImGui::DockBuilderRemoveNode(dockSpaceId);
    ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockSpaceId, viewport->WorkSize);

    ImGuiID center = dockSpaceId;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);

    ImGui::DockBuilderDockWindow(ViewportPanel::kWindowName, center);
    ImGui::DockBuilderDockWindow(LevelPanel::kWindowName, left);
    ImGui::DockBuilderDockWindow(InspectorPanel::kWindowName, right);
    ImGui::DockBuilderDockWindow(AssetLibraryPanel::kWindowName, bottom);
    ImGui::DockBuilderFinish(dockSpaceId);
}

void EditorApplication::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) { return; }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Level", "Ctrl+N")) { RequestAction(PendingAction::NewLevel); }
        if (ImGui::MenuItem("Open Level...", "Ctrl+O")) { RequestAction(PendingAction::OpenLevel); }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Level", "Ctrl+S")) { SaveLevel(); }
        if (ImGui::MenuItem("Save Level As...", "Ctrl+Shift+S")) { OpenSaveAsDialog(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) { RequestAction(PendingAction::Exit); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        const bool hasSelection = m_Context.GetSelectedObject() != nullptr;
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection)) { m_Context.DuplicateSelected(); }
        if (ImGui::MenuItem("Delete", "Del", false, hasSelection)) {
            m_Context.DeleteObject(m_Context.GetSelectedObject()->GetId());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Focus Selection", "F", false, hasSelection)) {
            m_Context.RequestFocus(m_Context.GetSelectedObject()->GetId());
        }
        if (ImGui::MenuItem("Frame Level")) { FocusLevel(); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Play")) {
        if (ImGui::MenuItem("Play in Game Window", "F5", m_Context.IsPlaying())) { TogglePlaying(); }
        ImGui::SetItemTooltip("Play the level (as it is now, unsaved changes included) in the game, in a window of its\n"
                              "own. F5 again, here or in the game, or closing the game's window, stops it.");

        if (ImGui::MenuItem("Simulate Physics Here", "Alt+S", m_Context.IsSimulating())) { ToggleSimulation(); }
        ImGui::SetItemTooltip("Run the level's physics in this viewport: simulated rigid bodies fall and collide.\n"
                              "Stopping puts everything back where it was.");

        ImGui::Separator();

        bool showColliders = m_ViewportPanel.GetShowColliders();
        if (ImGui::MenuItem("Show Colliders", nullptr, &showColliders)) { m_ViewportPanel.SetShowColliders(showColliders); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        if (ImGui::MenuItem("Reset Layout")) { m_ResetLayout = true; }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void EditorApplication::DrawDialogs() {
    if (const std::optional<std::filesystem::path> path = m_OpenLevelDialog.Draw()) { OpenLevel(*path); }

    if (const std::optional<std::filesystem::path> path = m_SaveLevelDialog.Draw()) {
        if (SaveLevelAs(*path) && m_AfterSave != PendingAction::None) { ExecuteAction(m_AfterSave); }
        m_AfterSave = PendingAction::None;
    }
    else if (!m_SaveLevelDialog.IsOpen()) {
        m_AfterSave = PendingAction::None;  // Cancelled: whatever was waiting on the save is off.
    }

    // Unsaved changes: Save / Don't Save / Cancel.
    if (m_OpenUnsavedPrompt) {
        ImGui::OpenPopup(kUnsavedPopup);
        m_OpenUnsavedPrompt = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kUnsavedPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("'%s' has unsaved changes.", m_Context.GetLevel().GetName().c_str());
        ImGui::Spacing();

        const PendingAction action = m_PendingAction;
        if (ImGui::Button("Save", ImVec2(110.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
            m_PendingAction = PendingAction::None;
            if (SaveLevel()) { ExecuteAction(action); }
            else { m_AfterSave = action; }  // Save As dialog is open; continue once it succeeds.
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
            m_PendingAction = PendingAction::None;
            ExecuteAction(action);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            m_PendingAction = PendingAction::None;
        }
        ImGui::EndPopup();
    }

    // Errors and warnings queued by any panel.
    if (!m_Context.GetErrors().empty() && !ImGui::IsPopupOpen(kMessagesPopup) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        ImGui::OpenPopup(kMessagesPopup);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(900.0f, 600.0f));
    if (ImGui::BeginPopupModal(kMessagesPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        for (const std::string& message : m_Context.GetErrors()) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", message.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_Context.ClearErrors();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApplication::HandleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();

    // Not while typing, flying, or in a dialog.
    if (io.WantTextInput || m_EditorCamera.IsControlling() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        return;
    }

    const auto pressed = [](const ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };

    if (pressed(ImGuiKey_F5)) { TogglePlaying(); }
    if (io.KeyAlt && pressed(ImGuiKey_S)) { ToggleSimulation(); }

    if (io.KeyCtrl) {
        if (pressed(ImGuiKey_N)) { RequestAction(PendingAction::NewLevel); }
        if (pressed(ImGuiKey_O)) { RequestAction(PendingAction::OpenLevel); }
        if (pressed(ImGuiKey_S)) {
            if (io.KeyShift) { OpenSaveAsDialog(); }
            else { SaveLevel(); }
        }
        if (pressed(ImGuiKey_D)) { m_Context.DuplicateSelected(); }
        return;
    }

    if (const Frost::GameObject* const selected = m_Context.GetSelectedObject(); selected != nullptr) {
        if (pressed(ImGuiKey_Delete)) { m_Context.DeleteObject(selected->GetId()); }
        else if (pressed(ImGuiKey_F)) { m_Context.RequestFocus(selected->GetId()); }
    }
}

void EditorApplication::RequestAction(const PendingAction action) {
    if (!m_Context.IsLevelDirty()) {
        ExecuteAction(action);
        return;
    }

    m_PendingAction = action;
    m_OpenUnsavedPrompt = true;
}

void EditorApplication::ExecuteAction(const PendingAction action) {
    switch (action) {
        case PendingAction::None: break;
        case PendingAction::NewLevel: m_Context.SetLevel(std::make_unique<Frost::Level>(), std::nullopt); break;
        case PendingAction::OpenLevel:
            m_OpenLevelDialog.Open(m_Context.GetLevelPath(), GetProject().GetLevelsDirectory());
            break;
        case PendingAction::OpenLevelFile: OpenLevel(m_PendingLevelFile); break;
        case PendingAction::Exit: GetEngine().GetWindow().SetShouldClose(true); break;
    }
}

bool EditorApplication::SaveLevel() {
    if (!m_Context.GetLevelPath().has_value()) {
        OpenSaveAsDialog();
        return false;
    }
    return SaveLevelAs(*m_Context.GetLevelPath());
}

void EditorApplication::OpenSaveAsDialog() {
    // A level that has a file starts next to it; a new one in the project's Levels folder, named
    // after the level.
    const std::string& name = m_Context.GetLevel().GetName();
    const bool named = !name.empty() && name != Frost::Level::kDefaultName;
    m_SaveLevelDialog.Open(m_Context.GetLevelPath(), GetProject().GetLevelsDirectory(), named ? name : "NewLevel");
}

bool EditorApplication::SaveLevelAs(const std::filesystem::path& path) {
    if (const std::string problem = CheckLevelPath(path, true); !problem.empty()) {
        m_Context.ReportError("Could not save the level: " + problem);
        return false;
    }

    // What the simulation moved is not part of the level: put it back before saving.
    m_Context.StopSimulation();

    // A level still carrying the default name takes the name it is first saved under.
    Frost::Level& level = m_Context.GetLevel();
    const std::string previousName = level.GetName();
    if (previousName == Frost::Level::kDefaultName) { level.SetName(Frost::PathToUtf8(path.stem())); }

    try {
        Frost::LevelSerializer::Save(level, path, &m_Library.GetRegistry());
    }
    catch (const Frost::LevelError& e) {
        level.SetName(previousName);
        m_Context.ReportError(std::string("Could not save the level: ") + e.what());
        return false;
    }

    m_Context.SetLevelSaved(Frost::AssetManager::NormalizePath(path));
    return true;
}

void EditorApplication::OpenLevel(const std::filesystem::path& path) {
    if (const std::string problem = CheckLevelPath(path, false); !problem.empty()) {
        m_Context.ReportError("Could not open the level: " + problem);
        return;
    }

    Frost::LevelLoadResult result;
    try {
        result = Frost::LevelSerializer::Load(path, GetEngine().GetAssetManager());
    }
    catch (const Frost::LevelError& e) {
        m_Context.ReportError(std::string("Could not open the level: ") + e.what());
        return;
    }

    m_Context.SetLevel(std::move(result.level), Frost::AssetManager::NormalizePath(path));
    for (const std::string& warning : result.warnings) { m_Context.ReportError("Warning: " + warning); }

    FocusLevel();
}

std::string EditorApplication::CheckLevelPath(const std::filesystem::path& path, const bool saving) {
    const Frost::Project& project = GetProject();
    if (project.IsInContent(path)) { return {}; }

    const std::string content = Frost::PathToUtf8(project.GetContentDirectory());
    if (saving) {
        return "Levels are part of the project: save it inside the project's content folder (" + content +
               "), e.g. in Levels.";
    }
    return "This level is not part of the project '" + project.GetName() + "' (it is outside " + content +
           "). Projects are self-contained: open the level's own project to edit it.";
}

const Frost::Project& EditorApplication::GetProject() { return GetEngine().GetProject(); }

void EditorApplication::OpenAssetEditor(const std::filesystem::path& path) {
    const std::filesystem::path normalized = Frost::AssetManager::NormalizePath(path);
    for (const std::unique_ptr<SkeletalAssetEditor>& editor : m_AssetEditors) {
        if (editor->GetPath() == normalized) {
            editor->Focus();
            return;
        }
    }
    try {
        m_AssetEditors.push_back(std::make_unique<SkeletalAssetEditor>(m_Context, normalized));
    }
    catch (const Frost::ImportError& e) {
        m_Context.ReportError(std::string("Could not open '") + Frost::PathToUtf8(normalized.filename()) + "': " + e.what());
    }
}

void EditorApplication::FocusObject(const Frost::ObjectId id) {
    const Frost::GameObject* const object = m_Context.GetLevel().FindObject(id);
    if (object == nullptr) { return; }

    if (const std::optional<Frost::Bounds> bounds = GetFramingBounds(*object); bounds.has_value()) {
        m_EditorCamera.Focus(bounds->GetCenter(), glm::length(bounds->GetSize()) * 0.5f);
    }
    else {
        m_EditorCamera.Focus(glm::vec3(object->GetWorldMatrix()[3]), 1.0f * Frost::kMeter);  // Empty object: frame its position.
    }
}

void EditorApplication::FocusLevel() {
    if (const std::optional<Frost::Bounds> bounds = m_Context.GetLevel().GetBounds(); bounds.has_value()) {
        m_EditorCamera.Focus(bounds->GetCenter(), glm::length(bounds->GetSize()) * 0.5f);
    }
}

void EditorApplication::TogglePlaying() {
    if (m_Context.IsPlaying()) { m_Context.StopPlaying(); }
    else { m_Context.StartPlaying(); }
}

void EditorApplication::ToggleSimulation() {
    if (m_Context.IsSimulating()) { m_Context.StopSimulation(); }
    else { m_Context.StartSimulation(); }
}

void EditorApplication::UpdateWindowTitle() {
    std::string title = "Frost Editor - " + GetProject().GetName() + " - " + m_Context.GetLevel().GetName();
    if (m_Context.IsLevelDirty()) { title += " *"; }
    if (m_Context.IsSimulating()) { title += " [Simulating]"; }
    if (m_Context.IsPlaying()) { title += " [Playing]"; }
    title += m_Context.GetLevelPath().has_value() ? " (" + Frost::PathToUtf8(*m_Context.GetLevelPath()) + ")"
                                                  : std::string(" (not saved)");

    // Only when it changes: setting a window title is a round trip to the window system.
    if (title != m_WindowTitle) {
        m_WindowTitle = title;
        GetEngine().GetWindow().SetTitle(title);
    }
}

void EditorApplication::SetUiInputEnabled(const bool enabled) {
    constexpr ImGuiConfigFlags kInputFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;

    ImGuiIO& io = ImGui::GetIO();
    if (enabled) { io.ConfigFlags &= ~kInputFlags; }
    else { io.ConfigFlags |= kInputFlags; }
}
