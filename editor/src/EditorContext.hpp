#pragma once

#include "AssetLibrary.hpp"

#include <Frost/Scene/Level.hpp>

#include <glm/vec3.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Frost {
    class Engine;
    class AssetManager;
}  // namespace Frost

// State shared by the editor's panels: the level being edited (and where it lives on disk), the
// selection, the asset library, and the actions several panels trigger (spawning, deleting,
// duplicating objects). Errors are queued here and shown by EditorApplication.
class EditorContext final {
public:
    EditorContext(Frost::Engine& engine, AssetLibrary& library);

    [[nodiscard]]
    Frost::Engine& GetEngine() noexcept;

    [[nodiscard]]
    Frost::AssetManager& GetAssets() noexcept;

    [[nodiscard]]
    AssetLibrary& GetLibrary() noexcept;

    // --- Level ------------------------------------------------------------------------------------

    [[nodiscard]]
    Frost::Level& GetLevel() noexcept;

    // Replaces the level being edited; `path` is where it was loaded from (nullopt for a new,
    // never-saved level). Clears the selection and the dirty flag.
    void SetLevel(std::unique_ptr<Frost::Level> level, std::optional<std::filesystem::path> path);

    [[nodiscard]]
    const std::optional<std::filesystem::path>& GetLevelPath() const noexcept;

    // After a successful save.
    void SetLevelSaved(const std::filesystem::path& path);

    [[nodiscard]]
    bool IsLevelDirty() const noexcept;

    // Records that the level has unsaved changes.
    void MarkLevelDirty() noexcept;

    // --- Selection ----------------------------------------------------------------------------------

    [[nodiscard]]
    Frost::GameObject* GetSelectedObject() noexcept;

    // 0 clears the selection.
    void Select(Frost::ObjectId id) noexcept;

    // --- Actions ------------------------------------------------------------------------------------

    // Places a new object showing the mesh asset at `path` (.fmesh, in the project's content), with
    // its bottom resting at `position`; the object is selected. On failure the error is reported
    // and null returned.
    Frost::GameObject* SpawnMesh(const std::filesystem::path& path, const glm::vec3& position);

    // Imports `source` into the asset at `destination` (replacing it, keeping its id; a model's
    // textures go to `textureFolder`, empty = the default), then reloads the assets the level
    // uses so it shows the result. Returns an error message, empty on success.
    [[nodiscard]]
    std::string ImportAsset(const std::filesystem::path& source, const std::filesystem::path& destination,
                            const std::filesystem::path& textureFolder);

    // ImportAsset from the asset's recorded source. Errors are reported.
    void ReimportAsset(const std::filesystem::path& assetPath);

    // Deletes an asset file; objects using it (a model) show as missing, materials using it (a
    // texture) fall back to defaults. Returns an error message, empty on success.
    [[nodiscard]]
    std::string DeleteAsset(const std::filesystem::path& assetPath);

    // Drops every loaded asset and reloads what the level uses (after assets were replaced).
    void ReloadAllAssets();

    // After the library noticed changes on disk: retries models/textures that could not be found.
    void OnAssetsChanged();

    // Places a new light of `type` for a drop at `surfacePoint` (lifted off the surface, and for
    // spots aimed down at it); the object is selected.
    Frost::GameObject& SpawnLight(Frost::LightType type, const glm::vec3& surfacePoint);

    void DeleteObject(Frost::ObjectId id);

    // Duplicates the selected object (offset slightly so the copy is visible) and selects the copy.
    void DuplicateSelected();

    // Where objects added without a drop location go (in front of the camera); kept up to date by
    // EditorApplication.
    void SetSpawnPoint(const glm::vec3& point) noexcept;

    [[nodiscard]]
    const glm::vec3& GetSpawnPoint() const noexcept;

    // Asks for a level file to be opened (handled by EditorApplication, which asks about unsaved
    // changes first).
    void RequestOpenLevel(const std::filesystem::path& path);

    [[nodiscard]]
    std::optional<std::filesystem::path> ConsumeOpenLevelRequest() noexcept;

    // Asks for the camera to frame an object (handled by EditorApplication).
    void RequestFocus(Frost::ObjectId id) noexcept;

    [[nodiscard]]
    std::optional<Frost::ObjectId> ConsumeFocusRequest() noexcept;

    // --- Errors -------------------------------------------------------------------------------------

    void ReportError(std::string message);

    [[nodiscard]]
    const std::vector<std::string>& GetErrors() const noexcept;

    void ClearErrors() noexcept;

private:
    Frost::Engine& m_Engine;
    AssetLibrary& m_Library;

    std::unique_ptr<Frost::Level> m_Level;
    std::optional<std::filesystem::path> m_LevelPath;
    bool m_LevelDirty = false;

    Frost::ObjectId m_Selection = 0;
    glm::vec3 m_SpawnPoint{0.0f};
    std::optional<Frost::ObjectId> m_FocusRequest;
    std::optional<std::filesystem::path> m_OpenLevelRequest;

    std::vector<std::string> m_Errors;
};
