#pragma once

#include "AssetLibrary.hpp"
#include "EditorCamera.hpp"
#include "EditorContext.hpp"
#include "Panels/AssetLibraryPanel.hpp"
#include "Panels/InspectorPanel.hpp"
#include "Panels/LevelPanel.hpp"
#include "Panels/ViewportPanel.hpp"
#include "Widgets/FileDialog.hpp"

#include <Frost/Application.hpp>
#include <Frost/Project/Project.hpp>

#include <imgui.h>

#include <filesystem>
#include <optional>
#include <string>

// The level editor. Works on one Frost project (see Frost::Project) at a time: its assets and
// levels, all inside the project's content directory. Runs the engine with ImGui enabled and lays
// out a docked workspace around the 3D viewport: the level's object list on the left, the selected object's properties on the
// right, and the asset library along the bottom. Levels are created, opened and saved from the
// File menu (or Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S).
//
// Editor-only tooling lives in this target; anything the game will also need (rendering,
// lighting, input, levels, assets, ...) belongs in the engine.
class EditorApplication final : public Frost::Application {
public:
    // Opens `project` (creating its content and Levels directories if missing): assets are
    // imported into its content directory and levels saved there (in Levels/ by default).
    explicit EditorApplication(const Frost::Project& project);
    ~EditorApplication() override = default;

protected:
    void OnUpdate(float deltaTime) override;
    void OnImGuiRender() override;

private:
    // Things that would throw away unsaved changes, and so may have to ask first.
    // (OpenLevelFile opens m_PendingLevelFile, e.g. a level double-clicked in the Asset Library.)
    enum class PendingAction { None, NewLevel, OpenLevel, OpenLevelFile, Exit };

    void DrawDockSpace();
    void BuildDefaultLayout(ImGuiID dockSpaceId);
    void DrawMenuBar();
    void DrawDialogs();
    void HandleShortcuts();

    // Runs `action` now, or asks about unsaved changes first.
    void RequestAction(PendingAction action);
    void ExecuteAction(PendingAction action);

    // Saves to the level's file, or asks where (Save As) if it has none yet. Returns whether the
    // level was saved right away.
    bool SaveLevel();
    void OpenSaveAsDialog();
    bool SaveLevelAs(const std::filesystem::path& path);
    void OpenLevel(const std::filesystem::path& path);

    // What is wrong with saving/opening a level at `path` (empty = nothing): levels must be part
    // of the project, i.e. inside its content directory.
    [[nodiscard]]
    std::string CheckLevelPath(const std::filesystem::path& path, bool saving);

    [[nodiscard]]
    const Frost::Project& GetProject();

    void FocusObject(Frost::ObjectId id);
    void FocusLevel();
    void UpdateWindowTitle();

    // While the camera has the mouse, ImGui must neither react to it (hover, clicks, wheel) nor
    // to the keyboard (WASD would otherwise type into a focused text field).
    static void SetUiInputEnabled(bool enabled);

    AssetLibrary m_Library;
    EditorContext m_Context;
    EditorCamera m_EditorCamera;

    ViewportPanel m_ViewportPanel;
    LevelPanel m_LevelPanel;
    InspectorPanel m_InspectorPanel;
    AssetLibraryPanel m_AssetLibraryPanel;

    FileDialog m_OpenLevelDialog;
    FileDialog m_SaveLevelDialog;

    PendingAction m_PendingAction = PendingAction::None;  // Waiting on the unsaved-changes prompt.
    PendingAction m_AfterSave = PendingAction::None;      // To run once a Save As completes.
    std::filesystem::path m_PendingLevelFile;             // For PendingAction::OpenLevelFile.
    bool m_OpenUnsavedPrompt = false;
    bool m_ResetLayout = false;
    std::string m_WindowTitle;
};
