#include "EditorContext.hpp"

#include "Widgets/ImGuiHelpers.hpp"

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Engine.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Scene/Hierarchy.hpp>
#include <Frost/Units.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <utility>

namespace {

    // (Re)loads a skeletal mesh component's model and animation from their references.
    void LoadSkeletalMeshAssets(Frost::SkeletalMeshComponent& mesh, Frost::AssetManager& assets) {
        const Frost::AssetReference meshReference = mesh.GetSkeletalMeshReference();
        if (meshReference.IsSet()) {
            try {
                mesh.SetSkeletalMesh(assets.LoadSkeletalMesh(meshReference));
            }
            catch (const Frost::ImportError&) {
                mesh.SetUnresolvedSkeletalMesh(meshReference);
            }
        }

        const Frost::AssetReference animationReference = mesh.GetAnimationReference();
        if (animationReference.IsSet()) {
            try {
                mesh.SetAnimation(assets.LoadAnimation(animationReference));
            }
            catch (const Frost::ImportError&) {
                mesh.SetUnresolvedAnimation(animationReference);
            }
        }

        mesh.InvalidateMaterials();
    }

    // (Re)loads the animations of an object's animation triggers; with `onlyMissing`, just those
    // that could not be loaded before.
    void LoadTriggerAnimations(Frost::AnimationTriggerComponent& component, Frost::AssetManager& assets, const bool onlyMissing) {
        for (Frost::AnimationTrigger& trigger : component.triggers) {
            if (!trigger.animationReference.IsSet() || (onlyMissing && trigger.animation != nullptr)) { continue; }
            try {
                trigger.animation = assets.LoadAnimation(trigger.animationReference);
            }
            catch (const Frost::ImportError&) {
                trigger.animation = nullptr;
            }
        }
    }

}  // namespace

EditorContext::EditorContext(Frost::Engine& engine, AssetLibrary& library)
    : m_Engine(engine), m_Library(library), m_Level(std::make_unique<Frost::Level>()) {}

Frost::Engine& EditorContext::GetEngine() noexcept { return m_Engine; }

Frost::AssetManager& EditorContext::GetAssets() noexcept { return m_Engine.GetAssetManager(); }

AssetLibrary& EditorContext::GetLibrary() noexcept { return m_Library; }

Frost::Level& EditorContext::GetLevel() noexcept { return *m_Level; }

void EditorContext::SetLevel(std::unique_ptr<Frost::Level> level, std::optional<std::filesystem::path> path) {
    m_Simulating = false;
    m_SimulationSnapshot.clear();

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
    object.AddComponent<Frost::MeshComponent>(asset);

    // Rest the model on the point rather than burying its lower half in whatever was hit.
    object.GetTransform().position = position - glm::vec3(0.0f, asset->bounds.min.y, 0.0f);

    m_Selection = object.GetId();
    MarkLevelDirty();
    return &object;
}

Frost::GameObject* EditorContext::SpawnSkeletalMesh(const std::filesystem::path& path, const glm::vec3& position) {
    if (!m_Library.IsInside(path)) {
        ReportError("'" + Frost::PathToUtf8(path.filename()) + "' is not part of this project. A project only uses its "
                    "own assets: copy the file (with its skeleton and the textures it uses) into the project's content "
                    "folder first.");
        return nullptr;
    }

    std::shared_ptr<const Frost::SkeletalMeshAsset> asset;
    try {
        asset = GetAssets().LoadSkeletalMesh(path);
    }
    catch (const Frost::ImportError& e) {
        ReportError(std::string("Could not load the skeletal mesh: ") + e.what());
        return nullptr;
    }

    Frost::GameObject& object = m_Level->CreateObject(Frost::PathToUtf8(asset->path.stem()));
    object.AddComponent<Frost::SkeletalMeshComponent>(asset);

    // Rest the model (in its rest pose) on the point.
    const float bottom = asset->drawable != nullptr ? asset->drawable->bounds.min.y : 0.0f;
    object.GetTransform().position = position - glm::vec3(0.0f, bottom, 0.0f);

    if (asset->skeleton == nullptr && asset->skeletonReference.IsSet()) {
        ReportError("'" + asset->name + "' is shown unposed: its skeleton could not be found.");
    }

    const Frost::ObjectId id = object.GetId();
    if (AddDefaultAttachments(id) > 0) {
        // Rest what it comes with (a magazine hanging below a weapon) on the point too.
        float lowest = position.y;
        for (const Frost::EntityId part : Frost::CollectSubtree(m_Level->GetRegistry(), id)) {
            if (const std::optional<Frost::Bounds> bounds = m_Level->FindObject(part)->GetWorldBounds()) {
                lowest = std::min(lowest, bounds->min.y);
            }
        }
        m_Level->FindObject(id)->GetTransform().position.y += position.y - lowest;
    }

    m_Selection = id;
    MarkLevelDirty();
    return m_Level->FindObject(id);
}

Frost::GameObject* EditorContext::SpawnModel(const std::filesystem::path& path, const glm::vec3& position) {
    switch (Frost::AssetManager::GetAssetType(path).value_or(Frost::AssetType::Texture)) {
        case Frost::AssetType::Mesh: return SpawnMesh(path, position);
        case Frost::AssetType::SkeletalMesh: return SpawnSkeletalMesh(path, position);
        default: break;
    }
    ReportError("'" + Frost::PathToUtf8(path.filename()) + "' is not a model that can be placed in a level.");
    return nullptr;
}

std::string EditorContext::ImportAsset(const std::filesystem::path& source, const std::filesystem::path& destination,
                                       const std::filesystem::path& textureFolder,
                                       const Frost::AssetImporter::Options& options) {
    Frost::AssetImporter::Result result;
    try {
        result = m_Library.Import(source, destination, textureFolder, options);
    }
    catch (const Frost::ImportError& e) {
        return std::string("Import failed: ") + e.what();
    }

    for (const std::string& warning : result.warnings) {
        ReportError("Imported '" + Frost::PathToUtf8(destination.filename()) + "', but: " + warning);
    }

    // Reimporting replaces assets that may be loaded (a model, its textures, a texture other
    // models use): reload everything the level uses so it shows the new versions.
    ReloadAllAssets();
    return {};
}

std::string EditorContext::ImportModel(const std::filesystem::path& source, const Frost::AssetImporter::ModelOptions& options) {
    Frost::AssetImporter::ModelResult result;
    try {
        result = m_Library.ImportModel(source, options);
    }
    catch (const Frost::ImportError& e) {
        return std::string("Import failed: ") + e.what();
    }

    for (const std::string& warning : result.warnings) {
        ReportError("Imported '" + Frost::PathToUtf8(source.filename()) + "', but: " + warning);
    }

    ReloadAllAssets();
    return {};
}

void EditorContext::ReimportAsset(const std::filesystem::path& assetPath) {
    const AssetLibrary::Entry* const entry = m_Library.FindByPath(assetPath);
    if (entry == nullptr || entry->sourcePath.empty()) {
        ReportError("Cannot reimport '" + Frost::PathToUtf8(assetPath.filename()) + "': its source is not recorded.");
        return;
    }
    if (!entry->sourceItem.empty() && entry->type != Frost::AssetType::Animation) {
        // Made from part of a model (one of its textures): reimport the model instead. (An
        // animation is reimported by itself: the importer finds the same animation again.)
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

    // Show the effect right away: objects using a deleted model or animation show it as "missing"
    // (they keep the reference, so restoring the file brings it back); textures fall back to their
    // defaults, and skeletal meshes whose skeleton went show their bind pose.
    const auto isDeleted = [&](const auto& asset) { return asset != nullptr && reference.id.IsValid() && asset->id == reference.id; };
    switch (type) {
        case Frost::AssetType::Mesh:
            GetAssets().Unload(reference);
            m_Level->GetRegistry().Each<Frost::MeshComponent>([&](Frost::EntityId, Frost::MeshComponent& mesh) {
                if (isDeleted(mesh.GetMesh())) { mesh.SetUnresolvedMesh(mesh.GetMeshReference()); }
            });
            break;

        case Frost::AssetType::SkeletalMesh:
            GetAssets().Unload(reference);
            m_Level->GetRegistry().Each<Frost::SkeletalMeshComponent>([&](Frost::EntityId, Frost::SkeletalMeshComponent& mesh) {
                if (isDeleted(mesh.GetSkeletalMesh())) { mesh.SetUnresolvedSkeletalMesh(mesh.GetSkeletalMeshReference()); }
            });
            break;

        case Frost::AssetType::Animation:
            GetAssets().Unload(reference);
            m_Level->GetRegistry().Each<Frost::SkeletalMeshComponent>([&](Frost::EntityId, Frost::SkeletalMeshComponent& mesh) {
                if (isDeleted(mesh.GetAnimation())) { mesh.SetUnresolvedAnimation(mesh.GetAnimationReference()); }
                if (isDeleted(mesh.GetOneShotAnimation())) { mesh.StopOnce(); }
            });
            m_Level->GetRegistry().Each<Frost::AnimationTriggerComponent>([&](Frost::EntityId, Frost::AnimationTriggerComponent& component) {
                for (Frost::AnimationTrigger& trigger : component.triggers) {
                    if (isDeleted(trigger.animation)) { trigger.animation = nullptr; }
                }
            });
            break;

        case Frost::AssetType::Skeleton:
        case Frost::AssetType::Texture: ReloadAllAssets(); break;
    }
    return {};
}

void EditorContext::ReloadAllAssets() {
    Frost::AssetManager& assets = GetAssets();
    assets.UnloadAll();

    m_Level->GetRegistry().Each<Frost::MeshComponent>([&](Frost::EntityId, Frost::MeshComponent& mesh) {
        const Frost::AssetReference reference = mesh.GetMeshReference();
        if (reference.IsSet()) {
            try {
                mesh.SetMesh(assets.LoadMesh(reference));
            }
            catch (const Frost::ImportError&) {
                mesh.SetUnresolvedMesh(reference);
            }
        }
        mesh.InvalidateMaterials();
    });

    m_Level->GetRegistry().Each<Frost::SkeletalMeshComponent>([&](Frost::EntityId, Frost::SkeletalMeshComponent& mesh) {
        mesh.StopOnce();
        LoadSkeletalMeshAssets(mesh, assets);
    });
    m_Level->GetRegistry().Each<Frost::AnimationTriggerComponent>([&](Frost::EntityId, Frost::AnimationTriggerComponent& component) {
        LoadTriggerAnimations(component, assets, false);
    });
}

void EditorContext::OnAssetsChanged() {
    Frost::AssetManager& assets = GetAssets();

    m_Level->GetRegistry().Each<Frost::MeshComponent>([&](Frost::EntityId, Frost::MeshComponent& mesh) {
        const Frost::AssetReference reference = mesh.GetMeshReference();

        // A model that could not be found (or was loaded without some of its textures) may be
        // complete now, e.g. after its file was moved back into the content directory.
        const bool incomplete = reference.IsSet() && (mesh.GetMesh() == nullptr || mesh.GetMesh()->missingTextures > 0);
        if (incomplete) {
            if (mesh.GetMesh() != nullptr) { assets.Unload(reference); }
            try {
                mesh.SetMesh(assets.LoadMesh(reference));
            }
            catch (const Frost::ImportError&) {
                mesh.SetUnresolvedMesh(reference);
            }
        }

        // Texture overrides are resolved (and a missing one retried) when materials rebuild.
        mesh.InvalidateMaterials();
    });

    m_Level->GetRegistry().Each<Frost::SkeletalMeshComponent>([&](Frost::EntityId, Frost::SkeletalMeshComponent& mesh) {
        // Likewise a skeletal mesh (or its skeleton, or textures), and the animation it is posed with.
        const std::shared_ptr<const Frost::SkeletalMeshAsset>& asset = mesh.GetSkeletalMesh();
        const bool incompleteMesh = mesh.GetSkeletalMeshReference().IsSet() &&
                                    (asset == nullptr || asset->missingTextures > 0 ||
                                     (asset->skeleton == nullptr && asset->skeletonReference.IsSet()));
        if (incompleteMesh || mesh.IsAnimationMissing()) {
            if (incompleteMesh && asset != nullptr) { assets.Unload(mesh.GetSkeletalMeshReference()); }
            LoadSkeletalMeshAssets(mesh, assets);
        }
        mesh.InvalidateMaterials();
    });

    m_Level->GetRegistry().Each<Frost::AnimationTriggerComponent>([&](Frost::EntityId, Frost::AnimationTriggerComponent& component) {
        LoadTriggerAnimations(component, assets, true);
    });
}

Frost::GameObject& EditorContext::SpawnLight(const Frost::LightType type, const glm::vec3& surfacePoint) {
    Frost::GameObject& object = m_Level->CreateObject(EditorUI::LightDisplayName(type));
    object.AddComponent<Frost::LightComponent>(Frost::LightComponent::CreateDefault(type));

    Frost::TransformComponent& transform = object.GetTransform();
    switch (type) {
        case Frost::LightType::Directional:
            // Only the direction matters; start like an afternoon sun (down and across).
            transform.position = surfacePoint + glm::vec3(0.0f, 2.0f * Frost::kMeter, 0.0f);
            transform.rotation = glm::vec3(-50.0f, -30.0f, 0.0f);
            break;

        case Frost::LightType::Point:
            // A little off the surface, so it lights it rather than sitting in it.
            transform.position = surfacePoint + glm::vec3(0.0f, 1.0f * Frost::kMeter, 0.0f);
            break;

        case Frost::LightType::Spot:
            // Hanging above the drop point, pointing straight down at it.
            transform.position = surfacePoint + glm::vec3(0.0f, 3.0f * Frost::kMeter, 0.0f);
            transform.rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
            break;
    }

    m_Selection = object.GetId();
    MarkLevelDirty();
    return object;
}

void EditorContext::DeleteObject(const Frost::ObjectId id) {
    if (m_Level->FindObject(id) == nullptr) { return; }

    // What is attached to it goes with it (the most deeply attached first).
    const std::vector<Frost::EntityId> subtree = Frost::CollectSubtree(m_Level->GetRegistry(), id);
    for (auto object = subtree.rbegin(); object != subtree.rend(); ++object) {
        m_Level->DestroyObject(*object);
        if (m_Selection == *object) { m_Selection = 0; }
    }

    // Animation triggers no longer show or hide them.
    m_Level->GetRegistry().Each<Frost::AnimationTriggerComponent>([&](Frost::EntityId, Frost::AnimationTriggerComponent& component) {
        for (Frost::AnimationTrigger& trigger : component.triggers) {
            std::erase_if(trigger.visibilityKeys, [&](const Frost::TriggerVisibilityKey& key) {
                return std::find(subtree.begin(), subtree.end(), key.object) != subtree.end();
            });
        }
    });
    MarkLevelDirty();
}

void EditorContext::DuplicateSelected() {
    const Frost::GameObject* const source = GetSelectedObject();
    if (source == nullptr) { return; }

    // The object and everything attached to it; the copies of attached objects are attached to the
    // copies of their parents.
    const std::vector<Frost::EntityId> subtree = Frost::CollectSubtree(m_Level->GetRegistry(), source->GetId());
    std::map<Frost::EntityId, Frost::EntityId> copies;
    for (const Frost::EntityId original : subtree) {
        const Frost::GameObject* const object = m_Level->FindObject(original);
        if (object == nullptr) { continue; }

        Frost::GameObject& copy = m_Level->DuplicateObject(*object);
        copies.emplace(original, copy.GetId());
        if (Frost::AttachmentComponent* const attachment = copy.TryGetComponent<Frost::AttachmentComponent>()) {
            if (const auto parent = copies.find(attachment->parent); parent != copies.end()) { attachment->parent = parent->second; }
        }
    }

    // The copies' animation triggers show and hide the copies of the objects they named.
    for (const auto& [original, copyId] : copies) {
        Frost::AnimationTriggerComponent* const triggers = m_Level->FindObject(copyId)->TryGetComponent<Frost::AnimationTriggerComponent>();
        if (triggers == nullptr) { continue; }
        triggers->playing.reset();
        for (Frost::AnimationTrigger& trigger : triggers->triggers) {
            for (Frost::TriggerVisibilityKey& key : trigger.visibilityKeys) {
                if (const auto target = copies.find(key.object); target != copies.end()) { key.object = target->second; }
            }
        }
    }

    Frost::GameObject* const copy = m_Level->FindObject(copies.at(source->GetId()));
    if (!Frost::IsAttached(m_Level->GetRegistry(), copy->GetId())) {
        // Nudge the copy sideways by the model's width, so it does not sit exactly inside the original.
        const std::optional<Frost::Bounds> bounds = m_Level->FindObject(subtree.front())->GetWorldBounds();
        copy->GetTransform().position.x += bounds.has_value() ? bounds->GetSize().x : 1.0f * Frost::kMeter;
    }

    m_Selection = copy->GetId();
    MarkLevelDirty();
}

bool EditorContext::AttachObject(const Frost::ObjectId child, const Frost::ObjectId parent, std::string bone, std::string slot) {
    Frost::GameObject* const object = m_Level->FindObject(child);
    if (object == nullptr || m_Level->FindObject(parent) == nullptr) { return false; }

    if (!Frost::CanAttach(m_Level->GetRegistry(), child, parent)) {
        ReportError("'" + object->GetName() + "' cannot be attached to " +
                    (child == parent ? std::string("itself") : "'" + m_Level->FindObject(parent)->GetName() + "': it is attached to it") +
                    ".");
        return false;
    }

    object->AddComponent<Frost::AttachmentComponent>(
            Frost::AttachmentComponent{.parent = parent, .bone = std::move(bone), .slot = std::move(slot), .socket = {}});
    object->GetTransform() = Frost::TransformComponent{};
    MarkLevelDirty();
    return true;
}

void EditorContext::DetachObject(const Frost::ObjectId child) {
    Frost::GameObject* const object = m_Level->FindObject(child);
    if (object == nullptr || !object->HasComponent<Frost::AttachmentComponent>()) { return; }

    const glm::mat4 world = object->GetWorldMatrix();
    object->RemoveComponent<Frost::AttachmentComponent>();
    object->GetTransform().SetFromMatrix(world);
    MarkLevelDirty();
}

Frost::GameObject* EditorContext::SpawnAttachedMesh(const std::filesystem::path& path, const Frost::ObjectId parent) {
    if (m_Level->FindObject(parent) == nullptr) { return nullptr; }

    Frost::GameObject* const object = SpawnMesh(path, glm::vec3(0.0f));
    if (object == nullptr) { return nullptr; }

    if (!AttachObject(object->GetId(), parent, {}, {})) {
        m_Level->DestroyObject(object->GetId());
        m_Selection = 0;
        return nullptr;
    }
    return object;
}

std::size_t EditorContext::AddDefaultAttachments(const Frost::ObjectId id) {
    Frost::GameObject* const object = m_Level->FindObject(id);
    const Frost::SkeletalMeshComponent* const mesh =
            object != nullptr ? object->TryGetComponent<Frost::SkeletalMeshComponent>() : nullptr;
    if (mesh == nullptr || mesh->GetSkeletalMesh() == nullptr) { return 0; }

    // Copied: creating objects adds components, which may move the skeletal mesh component.
    const std::vector<Frost::DefaultAttachment> attachments = mesh->GetSkeletalMesh()->defaultAttachments;
    for (const Frost::DefaultAttachment& attachment : attachments) {
        Frost::GameObject& part = m_Level->CreateObject(attachment.name.empty() ? "Attachment" : attachment.name);
        Frost::MeshComponent& partMesh = part.AddComponent<Frost::MeshComponent>();
        try {
            partMesh.SetMesh(GetAssets().LoadMesh(attachment.mesh));
        }
        catch (const Frost::ImportError& e) {
            partMesh.SetUnresolvedMesh(attachment.mesh);
            ReportError("'" + part.GetName() + "': " + e.what());
        }
        part.AddComponent<Frost::AttachmentComponent>(
                Frost::AttachmentComponent{.parent = id, .bone = attachment.bone, .slot = attachment.slot, .socket = {}});
    }

    if (!attachments.empty()) { MarkLevelDirty(); }
    return attachments.size();
}

std::string EditorContext::SetVisibilityKeys(const Frost::AssetReference& animation, std::vector<Frost::VisibilityKey> keys) {
    const std::optional<std::filesystem::path> path = m_Library.GetRegistry().Resolve(animation);
    if (!path.has_value()) { return "The animation could not be found."; }

    const std::filesystem::path projectDirectory = m_Library.GetRegistry().GetProjectDirectory();
    try {
        Frost::AnimationFile file = Frost::AssetFile::ReadAnimation(*path, projectDirectory);
        file.clip.visibilityKeys = std::move(keys);
        file.clip.SortVisibilityKeys();
        Frost::AssetFile::WriteAnimation(*path, file, projectDirectory);
    }
    catch (const Frost::ImportError& e) {
        return std::string("Could not save the animation: ") + e.what();
    }

    // Everything that uses the animation switches to the saved version, where it was in it.
    Frost::AssetManager& assets = GetAssets();
    assets.Unload(animation);
    std::shared_ptr<const Frost::AnimationAsset> saved;
    try {
        saved = assets.LoadAnimation(animation);
    }
    catch (const Frost::ImportError& e) {
        return std::string("Could not reload the animation: ") + e.what();
    }

    const auto isIt = [&](const std::shared_ptr<const Frost::AnimationAsset>& asset) {
        return asset != nullptr && (asset->id == saved->id || asset->path == saved->path);
    };
    m_Level->GetRegistry().Each<Frost::SkeletalMeshComponent>([&](Frost::EntityId, Frost::SkeletalMeshComponent& mesh) {
        if (isIt(mesh.GetAnimation())) {
            const float time = mesh.GetAnimationTime();
            mesh.SetAnimation(saved);
            mesh.SetAnimationTime(time);
        }
        if (isIt(mesh.GetOneShotAnimation())) {
            const bool paused = mesh.IsOneShotPaused();
            mesh.PlayOnce(saved, mesh.GetOneShotTime());
            mesh.SetOneShotPaused(paused);
        }
    });
    m_Level->GetRegistry().Each<Frost::AnimationTriggerComponent>([&](Frost::EntityId, Frost::AnimationTriggerComponent& component) {
        for (Frost::AnimationTrigger& trigger : component.triggers) {
            if (isIt(trigger.animation)) { trigger.animation = saved; }
        }
    });
    return {};
}

void EditorContext::UpdateAnimationPreviews(const float deltaTime) {
    // Only one-shots: a component's own animation is previewed by the Inspector.
    m_Level->GetRegistry().Each<Frost::SkeletalMeshComponent>([&](Frost::EntityId, Frost::SkeletalMeshComponent& mesh) {
        if (mesh.IsPlayingOnce()) { mesh.Advance(deltaTime); }
    });

    // A trigger's animation that has ended is no longer playing.
    Frost::Registry& registry = m_Level->GetRegistry();
    registry.Each<Frost::AnimationTriggerComponent>([&](const Frost::EntityId object, Frost::AnimationTriggerComponent& component) {
        if (component.playing.has_value() && component.GetPlaying(registry.TryGet<Frost::SkeletalMeshComponent>(object)) == nullptr) {
            component.playing.reset();
        }
    });
}

void EditorContext::SetSpawnPoint(const glm::vec3& point) noexcept { m_SpawnPoint = point; }

const glm::vec3& EditorContext::GetSpawnPoint() const noexcept { return m_SpawnPoint; }

void EditorContext::SetView(const Frost::ViewPoint& view) noexcept { m_View = view; }

const Frost::ViewPoint& EditorContext::GetView() const noexcept { return m_View; }

Frost::GameObject& EditorContext::SpawnCamera() {
    Frost::GameObject& object = m_Level->CreateObject("Camera");
    object.AddComponent<Frost::CameraComponent>();

    // Yaw then pitch, no roll: exactly the transform's Euler convention (R = Ry * Rx * Rz).
    Frost::TransformComponent& transform = object.GetTransform();
    transform.position = m_View.position;
    transform.rotation = glm::vec3(m_View.pitchDegrees, m_View.yawDegrees, 0.0f);

    m_Selection = object.GetId();
    MarkLevelDirty();
    return object;
}

void EditorContext::RequestOpenLevel(const std::filesystem::path& path) { m_OpenLevelRequest = path; }

std::optional<std::filesystem::path> EditorContext::ConsumeOpenLevelRequest() noexcept {
    std::optional<std::filesystem::path> request = std::move(m_OpenLevelRequest);
    m_OpenLevelRequest.reset();
    return request;
}

void EditorContext::RequestOpenAssetEditor(const std::filesystem::path& asset) { m_OpenAssetEditorRequest = asset; }

std::optional<std::filesystem::path> EditorContext::ConsumeOpenAssetEditorRequest() noexcept {
    std::optional<std::filesystem::path> request = std::move(m_OpenAssetEditorRequest);
    m_OpenAssetEditorRequest.reset();
    return request;
}

void EditorContext::RequestFocus(const Frost::ObjectId id) noexcept { m_FocusRequest = id; }

std::optional<Frost::ObjectId> EditorContext::ConsumeFocusRequest() noexcept {
    std::optional<Frost::ObjectId> request = m_FocusRequest;
    m_FocusRequest.reset();
    return request;
}

bool EditorContext::IsSimulating() const noexcept { return m_Simulating; }

void EditorContext::StartSimulation() {
    if (m_Simulating) { return; }

    m_SimulationSnapshot.clear();
    for (const std::unique_ptr<Frost::GameObject>& object : m_Level->GetObjects()) {
        m_SimulationSnapshot.emplace_back(object->GetId(), object->GetTransform());
    }

    // Everything starts at rest, with no time carried over from a previous run.
    m_Level->GetRegistry().Each<Frost::RigidBodyComponent>(
            [](Frost::EntityId, Frost::RigidBodyComponent& body) { body.linearVelocity = glm::vec3(0.0f); });
    m_Physics.Reset();

    m_Simulating = true;
}

void EditorContext::StopSimulation() {
    if (!m_Simulating) { return; }
    m_Simulating = false;

    // Objects deleted meanwhile are skipped; objects added meanwhile stay where they are.
    for (const auto& [id, transform] : m_SimulationSnapshot) {
        if (Frost::GameObject* const object = m_Level->FindObject(id); object != nullptr) {
            object->GetTransform() = transform;
        }
    }
    m_SimulationSnapshot.clear();

    m_Level->GetRegistry().Each<Frost::RigidBodyComponent>(
            [](Frost::EntityId, Frost::RigidBodyComponent& body) { body.linearVelocity = glm::vec3(0.0f); });
}

void EditorContext::UpdateSimulation(const float deltaTime) {
    if (m_Simulating) { m_Physics.Update(m_Level->GetRegistry(), deltaTime); }
}

Frost::GameObject& EditorContext::SpawnCharacter(const glm::vec3& point) {
    // About a person: 1.8 tall, eyes 0.7 above the middle (1.6 above the feet), 80 kg.
    const Frost::CapsuleColliderComponent capsule{.center = glm::vec3(0.0f),
                                                  .radius = 35.0f * Frost::kCentimeter,
                                                  .height = 1.8f * Frost::kMeter};

    Frost::GameObject& object = m_Level->CreateObject("Player");
    object.AddComponent<Frost::CapsuleColliderComponent>(capsule);
    object.AddComponent<Frost::RigidBodyComponent>(Frost::RigidBodyComponent{.simulated = true, .mass = 80.0f});
    object.AddComponent<Frost::CharacterControllerComponent>();
    object.AddComponent<Frost::CameraComponent>(Frost::CameraComponent{.offset = glm::vec3(0.0f, 70.0f * Frost::kCentimeter, 0.0f)});

    // Feet on the point, facing where the view faces, level.
    Frost::TransformComponent& transform = object.GetTransform();
    transform.position = point + glm::vec3(0.0f, capsule.height * 0.5f, 0.0f);
    transform.rotation = glm::vec3(0.0f, m_View.yawDegrees, 0.0f);

    m_Selection = object.GetId();
    MarkLevelDirty();
    return object;
}

bool EditorContext::IsPlaying() const noexcept { return m_PlaySession.IsRunning(); }

void EditorContext::StartPlaying() {
    if (m_PlaySession.IsRunning()) { return; }

    // The game plays the level as edited, not as the simulation left it.
    StopSimulation();

    const std::string error = m_PlaySession.Start(*m_Level, m_Engine.GetProject(), m_Library.GetRegistry(), m_View);
    if (!error.empty()) { ReportError("Could not play the level: " + error); }
}

void EditorContext::StopPlaying() { m_PlaySession.Stop(); }

void EditorContext::UpdatePlaying() {
    if (const std::optional<std::string> error = m_PlaySession.Update()) { ReportError(*error); }
}

void EditorContext::ReportError(std::string message) {
    std::cerr << "[Editor] " << message << '\n';
    m_Errors.push_back(std::move(message));
}

const std::vector<std::string>& EditorContext::GetErrors() const noexcept { return m_Errors; }

void EditorContext::ClearErrors() noexcept { m_Errors.clear(); }
