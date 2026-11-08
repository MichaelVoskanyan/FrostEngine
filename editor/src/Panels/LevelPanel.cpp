#include "LevelPanel.hpp"

#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Scene/Hierarchy.hpp>

#include <imgui.h>

#include <glm/vec3.hpp>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

void LevelPanel::Draw(EditorContext& context) {
    if (!ImGui::Begin(kWindowName)) {
        ImGui::End();
        return;
    }

    Frost::Level& level = context.GetLevel();

    std::string name = level.GetName();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (EditorUI::InputText("##LevelName", name)) {
        level.SetName(name);
        context.MarkLevelDirty();
    }

    if (ImGui::Button("Add...")) { ImGui::OpenPopup("AddObject"); }
    if (ImGui::BeginPopup("AddObject")) {
        if (ImGui::MenuItem("Empty Object")) {
            Frost::GameObject& object = level.CreateObject("Empty");
            object.GetTransform().position = context.GetSpawnPoint();
            context.Select(object.GetId());
            context.MarkLevelDirty();
        }
        if (ImGui::MenuItem("Camera")) { context.SpawnCamera(); }
        ImGui::SetItemTooltip("A camera where the viewport is looking from. The game looks through the level's camera.");
        if (ImGui::MenuItem("Character")) { context.SpawnCharacter(context.GetSpawnPoint()); }
        ImGui::SetItemTooltip("A player character to walk around the level as in the game: a capsule body that falls and\n"
                              "collides, walks with W/A/S/D, jumps with Space and looks with the mouse through a camera\n"
                              "at head height.");
        ImGui::Separator();
        for (const Frost::LightType type : {Frost::LightType::Directional, Frost::LightType::Point, Frost::LightType::Spot}) {
            if (ImGui::MenuItem(EditorUI::LightDisplayName(type))) { context.SpawnLight(type, context.GetSpawnPoint()); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Audio Source")) { context.SpawnAudioSource({}, context.GetSpawnPoint()); }
        ImGui::SetItemTooltip("An object that plays a sound (pick it in the Inspector). Or drag a sound from the Asset\n"
                              "Library into the viewport or this list.");
        ImGui::EndPopup();
    }

    // Level-wide lighting. Ambient light stands in for indirect/sky light until the renderer has
    // some; it keeps surfaces no light reaches from being pure black.
    if (ImGui::CollapsingHeader("Environment")) {
        Frost::LevelEnvironment& environment = level.GetEnvironment();

        glm::vec3 ambient = EditorUI::LinearToSrgb(environment.ambientColor);
        if (ImGui::ColorEdit3("Ambient", &ambient.x)) {
            environment.ambientColor = EditorUI::SrgbToLinear(ambient);
            context.MarkLevelDirty();
        }
        if (ImGui::DragFloat("Intensity##Ambient", &environment.ambientIntensity, 0.005f, 0.0f, 10.0f, "%.3f",
                             ImGuiSliderFlags_AlwaysClamp)) {
            context.MarkLevelDirty();
        }
        ImGui::SetItemTooltip("Flat light added to every surface (0 = none).");
    }

    ImGui::Separator();

    // Actions are applied after the loop: they create/destroy objects and change attachments, which
    // would invalidate it.
    std::optional<Frost::ObjectId> toDelete;
    std::optional<Frost::ObjectId> toDetach;
    std::optional<std::pair<Frost::ObjectId, Frost::ObjectId>> toAttach;                   // Child, parent.
    std::optional<std::pair<std::filesystem::path, Frost::ObjectId>> toSpawnAttached;  // Mesh, parent.
    bool duplicate = false;

    if (ImGui::BeginChild("##Objects")) {
        const Frost::GameObject* const selected = context.GetSelectedObject();
        const Frost::Registry& registry = level.GetRegistry();

        // The objects as a tree: each is listed under the object it is attached to.
        std::map<Frost::ObjectId, std::vector<const Frost::GameObject*>> children;
        std::vector<const Frost::GameObject*> roots;
        for (const std::unique_ptr<Frost::GameObject>& object : level.GetObjects()) {
            if (Frost::IsAttached(registry, object->GetId())) {
                children[object->GetComponent<Frost::AttachmentComponent>().parent].push_back(object.get());
            }
            else {
                roots.push_back(object.get());
            }
        }

        std::set<Frost::ObjectId> listed;
        const auto drawObject = [&](const auto& self, const Frost::GameObject& object) -> void {
            if (!listed.insert(object.GetId()).second) { return; }
            ImGui::PushID(static_cast<int>(object.GetId()));

            // Objects whose model (or animation) failed to load are flagged, so broken references are
            // easy to spot.
            std::optional<std::filesystem::path> missingAsset;
            bool hidden = false;
            if (const Frost::MeshComponent* const mesh = object.TryGetComponent<Frost::MeshComponent>()) {
                if (mesh->IsMissing()) { missingAsset = mesh->GetMeshReference().path; }
                hidden = !mesh->IsVisible();
            }
            if (const Frost::SkeletalMeshComponent* const mesh = object.TryGetComponent<Frost::SkeletalMeshComponent>()) {
                if (mesh->IsMissing()) { missingAsset = mesh->GetSkeletalMeshReference().path; }
                else if (mesh->IsAnimationMissing()) { missingAsset = mesh->GetAnimationReference().path; }
                hidden = hidden || !mesh->IsVisible();
            }
            if (const Frost::AnimationTriggerComponent* const triggers = object.TryGetComponent<Frost::AnimationTriggerComponent>()) {
                for (const Frost::AnimationTrigger& trigger : triggers->triggers) {
                    if (trigger.animationReference.IsSet() && trigger.animation == nullptr) { missingAsset = trigger.animationReference.path; }
                }
            }
            const std::string label = object.GetName().empty() ? "(unnamed)" : object.GetName();

            const auto found = children.find(object.GetId());
            const bool hasChildren = found != children.end() && !found->second.empty();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding;
            if (!hasChildren) { flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen; }
            if (&object == selected) { flags |= ImGuiTreeNodeFlags_Selected; }

            if (missingAsset.has_value()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f)); }
            else if (hidden) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); }
            const bool open = ImGui::TreeNodeEx("##Object", flags, "%s", label.c_str());
            if (missingAsset.has_value() || hidden) { ImGui::PopStyleColor(); }

            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                context.Select(object.GetId());
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { context.RequestFocus(object.GetId()); }
            }
            if (missingAsset.has_value()) { ImGui::SetItemTooltip("Not found: %s", Frost::PathToUtf8(*missingAsset).c_str()); }
            else if (const Frost::AttachmentComponent* const attachment = object.TryGetComponent<Frost::AttachmentComponent>();
                     attachment != nullptr && Frost::IsAttached(registry, object.GetId())) {
                const std::string mount = !attachment->socket.empty() ? " to socket '" + attachment->socket + "'"
                                          : !attachment->bone.empty()   ? " to bone '" + attachment->bone + "'"
                                                                        : std::string();
                ImGui::SetItemTooltip("Attached%s%s%s", mount.c_str(), attachment->slot.empty() ? "" : ", slot ", attachment->slot.c_str());
            }

            // Drag an object onto another to attach it there; drop a model onto one to attach a new
            // object showing it.
            EditorUI::ObjectDragSource(object.GetId(), label.c_str());
            if (ImGui::BeginDragDropTarget()) {
                if (const std::optional<Frost::EntityId> dropped = EditorUI::AcceptObjectPayload(); dropped.has_value() && *dropped != object.GetId()) {
                    toAttach = std::make_pair(*dropped, object.GetId());
                }
                if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetPayload(Frost::AssetType::Mesh)) {
                    toSpawnAttached = std::make_pair(*dropped, object.GetId());
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextItem()) {
                context.Select(object.GetId());
                if (ImGui::MenuItem("Focus", "F")) { context.RequestFocus(object.GetId()); }
                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) { duplicate = true; }
                if (object.HasComponent<Frost::AttachmentComponent>() && ImGui::MenuItem("Detach")) { toDetach = object.GetId(); }
                if (ImGui::MenuItem("Delete", "Del")) { toDelete = object.GetId(); }
                ImGui::EndPopup();
            }

            // Lights and cameras have no geometry to recognize them by; tag them (lights dimmed
            // when switched off), and hidden models. After the context menu, which attaches to the
            // last item (the tree node).
            const char* tag = nullptr;
            if (object.HasComponent<Frost::CharacterControllerComponent>()) {
                tag = "character";
            }
            else if (const Frost::LightComponent* const light = object.TryGetComponent<Frost::LightComponent>(); light != nullptr) {
                tag = light->enabled ? "light" : "light (off)";
            }
            else if (object.HasComponent<Frost::CameraComponent>()) {
                tag = "camera";
            }
            else if (hidden) {
                tag = "hidden";
            }
            if (tag != nullptr) {
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(tag).x);
                ImGui::TextDisabled("%s", tag);
            }

            if (hasChildren && open) {
                for (const Frost::GameObject* const child : found->second) { self(self, *child); }
                ImGui::TreePop();
            }
            ImGui::PopID();
        };

        for (const Frost::GameObject* const object : roots) { drawObject(drawObject, *object); }
        // (Anything left would be part of an attachment loop, which loading and attaching prevent.)
        for (const std::unique_ptr<Frost::GameObject>& object : level.GetObjects()) { drawObject(drawObject, *object); }

        if (level.GetObjects().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("No objects. Drag a model from the Asset Library into the viewport or this list.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    // The whole list area accepts models (static and skeletal), sounds (as audio sources), and
    // objects dragged out of others (detaching them).
    if (ImGui::BeginDragDropTarget()) {
        for (const Frost::AssetType type : {Frost::AssetType::Mesh, Frost::AssetType::SkeletalMesh}) {
            if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetPayload(type)) {
                context.SpawnModel(*dropped, context.GetSpawnPoint());
            }
        }
        if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetPayload(Frost::AssetType::Sound)) {
            context.SpawnAudioSource(*dropped, context.GetSpawnPoint());
        }
        if (const std::optional<Frost::EntityId> dropped = EditorUI::AcceptObjectPayload()) { toDetach = *dropped; }
        ImGui::EndDragDropTarget();
    }

    if (toAttach.has_value()) {
        // Onto its own parent again: nothing to do.
        const Frost::GameObject* const child = level.FindObject(toAttach->first);
        const Frost::AttachmentComponent* const current = child != nullptr ? child->TryGetComponent<Frost::AttachmentComponent>() : nullptr;
        if (child != nullptr && (current == nullptr || current->parent != toAttach->second)) {
            // It keeps the role it had (a slot), but a bone belongs to the previous parent.
            context.AttachObject(toAttach->first, toAttach->second, {}, current != nullptr ? current->slot : std::string());
            context.Select(toAttach->first);
        }
    }
    if (toSpawnAttached.has_value()) { context.SpawnAttachedMesh(toSpawnAttached->first, toSpawnAttached->second); }
    if (toDetach.has_value()) { context.DetachObject(*toDetach); }
    if (duplicate) { context.DuplicateSelected(); }
    if (toDelete.has_value()) { context.DeleteObject(*toDelete); }

    ImGui::End();
}
