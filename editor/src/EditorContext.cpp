#include "EditorContext.hpp"

#include "Widgets/ImGuiHelpers.hpp"

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Engine.hpp>
#include <Frost/Platform/Filesystem.hpp>

#include <iostream>
#include <utility>

EditorContext::EditorContext(Frost::Engine& engine, AssetLibrary& library)
    : m_Engine(engine), m_Library(library), m_Level(std::make_unique<Frost::Level>()) {}

Frost::Engine& EditorContext::GetEngine() noexcept { return m_Engine; }

Frost::AssetManager& EditorContext::GetAssets() noexcept { return m_Engine.GetAssetManager(); }

AssetLibrary& EditorContext::GetLibrary() noexcept { return m_Library; }

Frost::Level& EditorContext::GetLevel() noexcept { return *m_Level; }

void EditorContext::SetLevel(std::unique_ptr<Frost::Level> level, std::optional<std::filesystem::path> path) {
    m_Level = std::move(level);
    m_LevelPath = std::move(path);
    m_LevelDirty = false;
    m_Selection = 0;
    m_FocusRequest.reset();
}

const std::optional<std::filesystem::path>& EditorContext::GetLevelPath() const noexcept { return m_LevelPath; }

void EditorContext::SetLevelSaved(const std::filesystem::path& path) {
    m_LevelPath = path;
    m_LevelDirty = false;
}

bool EditorContext::IsLevelDirty() const noexcept { return m_LevelDirty; }

void EditorContext::MarkLevelDirty() noexcept { m_LevelDirty = true; }

Frost::GameObject* EditorContext::GetSelectedObject() noexcept {
    return m_Selection != 0 ? m_Level->FindObject(m_Selection) : nullptr;
}

void EditorContext::Select(const Frost::ObjectId id) noexcept { m_Selection = id; }

Frost::GameObject* EditorContext::SpawnMesh(const std::filesystem::path& path, const glm::vec3& position) {
    if (!m_Library.IsInside(path)) {
        ReportError("'" + Frost::PathToUtf8(path.filename()) + "' is not part of this project. A project only uses its "
                    "own assets: copy the file (and the textures it uses) into the project's content folder first.");
        return nullptr;
    }

    std::shared_ptr<const Frost::MeshAsset> asset;
    try {
        asset = GetAssets().LoadMesh(path);
    }
    catch (const Frost::ImportError& e) {
        ReportError(std::string("Could not load the model: ") + e.what());
        return nullptr;
    }

    Frost::GameObject& object = m_Level->CreateObject(Frost::PathToUtf8(asset->path.stem()));
    object.SetMesh(asset);

    // Rest the model on the point rather than burying its lower half in whatever was hit.
    object.GetTransform().position = position - glm::vec3(0.0f, asset->bounds.min.y, 0.0f);

    m_Selection = object.GetId();
    MarkLevelDirty();
    return &object;
}

std::string EditorContext::ImportAsset(const std::filesystem::path& source, const std::filesystem::path& destination,
                                       const std::filesystem::path& textureFolder) {
    try {
        static_cast<void>(m_Library.Import(source, destination, textureFolder));
    }
    catch (const Frost::ImportError& e) {
        return std::string("Import failed: ") + e.what();
    }

    // Reimporting replaces assets that may be loaded (a model, its textures, a texture other
    // models use): reload everything the level uses so it shows the new versions.
    ReloadAllAssets();
    return {};
}

void EditorContext::ReimportAsset(const std::filesystem::path& assetPath) {
    const AssetLibrary::Entry* const entry = m_Library.FindByPath(assetPath);
    if (entry == nullptr || entry->sourcePath.empty()) {
        ReportError("Cannot reimport '" + Frost::PathToUtf8(assetPath.filename()) + "': its source is not recorded.");
        return;
    }
    if (!entry->sourceItem.empty()) {
        // Made from part of a model (one of its textures): reimport the model instead.
        ReportError("'" + Frost::PathToUtf8(assetPath.filename()) + "' was imported as part of '" +
                    Frost::PathToUtf8(entry->sourcePath.filename()) + "'; reimport that model to update it.");
        return;
    }

    // Copied: the import rescans the library, invalidating `entry`.
    const std::filesystem::path source = entry->sourcePath;
    const std::filesystem::path path = entry->path;

    if (const std::string error = ImportAsset(source, path, {}); !error.empty()) { ReportError(error); }
}

std::string EditorContext::DeleteAsset(const std::filesystem::path& assetPath) {
    const AssetLibrary::Entry* const entry = m_Library.FindByPath(assetPath);
    if (entry == nullptr) { return "'" + Frost::PathToUtf8(assetPath.filename()) + "' is not an asset in the library."; }

    const Frost::AssetReference reference{.id = entry->id, .path = entry->path};
    const Frost::AssetType type = entry->type;

    if (std::string error = m_Library.DeleteAsset(assetPath); !error.empty()) { return error; }

    // Show the effect right away: objects using a deleted model become "missing" (they keep the
    // reference, so restoring the file brings them back); textures fall back to their defaults.
    if (type == Frost::AssetType::Mesh) {
        GetAssets().Unload(reference);
        for (const std::unique_ptr<Frost::GameObject>& object : m_Level->GetObjects()) {
            if (object->GetMesh() != nullptr && reference.id.IsValid() && object->GetMesh()->id == reference.id) {
                object->SetUnresolvedMesh(object->GetMeshReference());
            }
        }
    }
    else {
        ReloadAllAssets();
    }
    return {};
}

void EditorContext::ReloadAllAssets() {
    Frost::AssetManager& assets = GetAssets();
    assets.UnloadAll();

    for (const std::unique_ptr<Frost::GameObject>& object : m_Level->GetObjects()) {
        const Frost::AssetReference reference = object->GetMeshReference();
        if (reference.IsSet()) {
            try {
                object->SetMesh(assets.LoadMesh(reference));
            }
            catch (const Frost::ImportError&) {
                object->SetUnresolvedMesh(reference);
            }
        }
        object->InvalidateMaterials();
    }
}

void EditorContext::OnAssetsChanged() {
    Frost::AssetManager& assets = GetAssets();

    for (const std::unique_ptr<Frost::GameObject>& object : m_Level->GetObjects()) {
        const Frost::AssetReference reference = object->GetMeshReference();

        // A model that could not be found (or was loaded without some of its textures) may be
        // complete now, e.g. after its file was moved back into the content directory.
        const bool incomplete = reference.IsSet() && (object->GetMesh() == nullptr || object->GetMesh()->missingTextures > 0);
        if (incomplete) {
            if (object->GetMesh() != nullptr) { assets.Unload(reference); }
            try {
                object->SetMesh(assets.LoadMesh(reference));
            }
            catch (const Frost::ImportError&) {
                object->SetUnresolvedMesh(reference);
            }
        }

        // Texture overrides are resolved (and a missing one retried) when materials rebuild.
        object->InvalidateMaterials();
    }
}

Frost::GameObject& EditorContext::SpawnLight(const Frost::LightType type, const glm::vec3& surfacePoint) {
    Frost::GameObject& object = m_Level->CreateObject(EditorUI::LightDisplayName(type));
    object.GetLight() = Frost::LightComponent::CreateDefault(type);

    Frost::Transform& transform = object.GetTransform();
    switch (type) {
        case Frost::LightType::Directional:
            // Only the direction matters; start like an afternoon sun (down and across).
            transform.position = surfacePoint + glm::vec3(0.0f, 2.0f, 0.0f);
            transform.rotation = glm::vec3(-50.0f, -30.0f, 0.0f);
            break;

        case Frost::LightType::Point:
            // A little off the surface, so it lights it rather than sitting in it.
            transform.position = surfacePoint + glm::vec3(0.0f, 1.0f, 0.0f);
            break;

        case Frost::LightType::Spot:
            // Hanging above the drop point, pointing straight down at it.
            transform.position = surfacePoint + glm::vec3(0.0f, 3.0f, 0.0f);
            transform.rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
            break;
    }

    m_Selection = object.GetId();
    MarkLevelDirty();
    return object;
}

void EditorContext::DeleteObject(const Frost::ObjectId id) {
    if (m_Level->FindObject(id) == nullptr) { return; }

    m_Level->DestroyObject(id);
    if (m_Selection == id) { m_Selection = 0; }
    MarkLevelDirty();
}

void EditorContext::DuplicateSelected() {
    const Frost::GameObject* const source = GetSelectedObject();
    if (source == nullptr) { return; }

    Frost::GameObject& copy = m_Level->DuplicateObject(*source);

    // Nudge the copy sideways by the model's width, so it does not sit exactly inside the original.
    const std::optional<Frost::Bounds> bounds = source->GetWorldBounds();
    copy.GetTransform().position.x += bounds.has_value() ? bounds->GetSize().x : 1.0f;

    m_Selection = copy.GetId();
    MarkLevelDirty();
}

void EditorContext::SetSpawnPoint(const glm::vec3& point) noexcept { m_SpawnPoint = point; }

const glm::vec3& EditorContext::GetSpawnPoint() const noexcept { return m_SpawnPoint; }

void EditorContext::RequestOpenLevel(const std::filesystem::path& path) { m_OpenLevelRequest = path; }

std::optional<std::filesystem::path> EditorContext::ConsumeOpenLevelRequest() noexcept {
    std::optional<std::filesystem::path> request = std::move(m_OpenLevelRequest);
    m_OpenLevelRequest.reset();
    return request;
}

void EditorContext::RequestFocus(const Frost::ObjectId id) noexcept { m_FocusRequest = id; }

std::optional<Frost::ObjectId> EditorContext::ConsumeFocusRequest() noexcept {
    std::optional<Frost::ObjectId> request = m_FocusRequest;
    m_FocusRequest.reset();
    return request;
}

void EditorContext::ReportError(std::string message) {
    std::cerr << "[Editor] " << message << '\n';
    m_Errors.push_back(std::move(message));
}

const std::vector<std::string>& EditorContext::GetErrors() const noexcept { return m_Errors; }

void EditorContext::ClearErrors() noexcept { m_Errors.clear(); }
