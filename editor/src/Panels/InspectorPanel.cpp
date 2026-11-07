#include "InspectorPanel.hpp"

#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Platform/KeyNames.hpp>
#include <Frost/Renderer/MaterialSystem.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Scene/Hierarchy.hpp>
#include <Frost/Units.hpp>

#include <imgui.h>

#include <glm/common.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    constexpr ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
    constexpr ImVec4 kWarningColor(1.0f, 0.75f, 0.35f, 1.0f);

    // Scale components are kept away from zero: a zero scale collapses the object (and makes its
    // transform singular).
    constexpr float kMinScale = 0.001f;

    [[nodiscard]]
    const char* SlotLabel(const Frost::MaterialSlot slot) {
        switch (slot) {
            case Frost::MaterialSlot::BaseColor: return "Base Color";
            case Frost::MaterialSlot::MetallicRoughness: return "Metal/Rough";
            case Frost::MaterialSlot::Normal: return "Normal";
            case Frost::MaterialSlot::Occlusion: return "Occlusion";
            case Frost::MaterialSlot::Emissive: return "Emissive";
        }
        return "?";
    }

    // Where a referenced asset is now (the registry follows renames and moves), if it can be
    // found at all.
    [[nodiscard]]
    std::optional<std::filesystem::path> CurrentPath(EditorContext& context, const Frost::AssetReference& reference) {
        return context.GetLibrary().GetRegistry().Resolve(reference);
    }

    // A reference to the asset file at `path` (its id from the library, or from the file).
    [[nodiscard]]
    Frost::AssetReference ReferenceTo(EditorContext& context, const std::filesystem::path& path) {
        const std::filesystem::path normalized = Frost::AssetManager::NormalizePath(path);
        if (const AssetLibrary::Entry* const entry = context.GetLibrary().FindByPath(normalized); entry != nullptr) {
            return Frost::AssetReference{.id = entry->id, .path = entry->path};
        }
        const std::optional<Frost::AssetHeader> header =
                Frost::AssetFile::ReadHeader(normalized, context.GetLibrary().GetRegistry().GetProjectDirectory());
        return Frost::AssetReference{.id = header.has_value() ? header->id : Frost::AssetId{}, .path = normalized};
    }

    // What a component added from the "Add Component" menu starts as (lights are created by type,
    // see DrawAddComponentMenu).
    template <typename T>
    [[nodiscard]]
    T MakeNewComponent(const Frost::GameObject& /*object*/) {
        return T{};
    }

    // The box of the object's model in its own space (its mesh's, else its skeletal mesh's in its
    // current pose), if it has one loaded. What colliders are fitted to.
    [[nodiscard]]
    std::optional<Frost::Bounds> ModelBounds(const Frost::GameObject& object) {
        if (const Frost::MeshComponent* const mesh = object.TryGetComponent<Frost::MeshComponent>()) {
            if (const std::optional<Frost::Bounds> bounds = mesh->GetLocalBounds()) { return bounds; }
        }
        if (const Frost::SkeletalMeshComponent* const mesh = object.TryGetComponent<Frost::SkeletalMeshComponent>()) {
            return mesh->GetLocalBounds();
        }
        return std::nullopt;
    }

    // A collider starts out wrapped around the object's model, if it has one.
    template <>
    [[nodiscard]]
    Frost::BoxColliderComponent MakeNewComponent<Frost::BoxColliderComponent>(const Frost::GameObject& object) {
        if (const std::optional<Frost::Bounds> bounds = ModelBounds(object)) { return Frost::BoxColliderComponent::FromBounds(*bounds); }
        return Frost::BoxColliderComponent{};
    }

    // A capsule likewise stands in the object's model, if it has one.
    template <>
    [[nodiscard]]
    Frost::CapsuleColliderComponent MakeNewComponent<Frost::CapsuleColliderComponent>(const Frost::GameObject& object) {
        if (const std::optional<Frost::Bounds> bounds = ModelBounds(object)) {
            return Frost::CapsuleColliderComponent::FromBounds(*bounds);
        }
        return Frost::CapsuleColliderComponent{};
    }

    // A model's "Visible" checkbox (MeshComponent, SkeletalMeshComponent), and what an animation
    // says about it right now. Returns whether it changed.
    template <typename Component>
    bool DrawVisibility(Component& mesh) {
        bool visible = mesh.IsVisible();
        const bool changed = ImGui::Checkbox("Visible", &visible);
        if (changed) { mesh.SetVisible(visible); }
        ImGui::SetItemTooltip("Whether the model is drawn (and can be clicked in the viewport). The animations of the\n"
                              "object it is attached to can show or hide it while they play (see visibility keys).");
        if (mesh.GetAnimationVisibility().has_value() && *mesh.GetAnimationVisibility() != visible) {
            ImGui::SameLine();
            ImGui::TextDisabled(*mesh.GetAnimationVisibility() ? "(shown by an animation now)" : "(hidden by an animation now)");
        }
        return changed;
    }

    // "1.25 s".
    [[nodiscard]]
    std::string FormatSeconds(const float seconds) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f s", static_cast<double>(seconds));
        return text;
    }

    // The slots (roles) that mean something on `object`: those of the objects attached to it, and
    // those the visibility keys of its skeletal mesh's own animation name.
    [[nodiscard]]
    std::vector<std::string> KnownSlots(const Frost::Registry& registry, const Frost::EntityId object) {
        std::set<std::string> slots;
        for (const Frost::EntityId child : Frost::GetAttachedObjects(registry, object)) {
            if (const Frost::AttachmentComponent* const attachment = registry.TryGet<Frost::AttachmentComponent>(child)) {
                if (!attachment->slot.empty()) { slots.insert(attachment->slot); }
            }
        }

        const auto addSlotsOf = [&slots](const Frost::AnimationAsset* const animation) {
            if (animation == nullptr) { return; }
            for (std::string& slot : animation->clip.GetVisibilitySlots()) { slots.insert(std::move(slot)); }
        };
        if (const Frost::SkeletalMeshComponent* const mesh = registry.TryGet<Frost::SkeletalMeshComponent>(object)) {
            addSlotsOf(mesh->GetAnimation().get());
        }
        return {slots.begin(), slots.end()};
    }

    // A text field for a slot name, with a menu of `known` ones beside it. Returns whether it
    // changed; `committed` is set when an edit is done (the field left, or a slot picked).
    bool SlotField(const char* const label, std::string& slot, const std::vector<std::string>& known, bool& committed) {
        bool changed = false;
        ImGui::PushID(label);
        const float buttonWidth = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(std::max(ImGui::CalcItemWidth() - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x, 1.0f));
        changed |= EditorUI::InputText("##Slot", slot);
        committed |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::BeginCombo("##Known", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
            if (known.empty()) { ImGui::TextDisabled("No slots in use yet."); }
            for (const std::string& name : known) {
                if (ImGui::Selectable(name.c_str(), name == slot)) {
                    slot = name;
                    changed = true;
                    committed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (label[0] != '#') {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextUnformatted(label);
        }
        ImGui::PopID();
        return changed;
    }

}  // namespace

template <typename T>
bool InspectorPanel::DrawComponentSection(EditorContext& context, Frost::GameObject& object,
                                          [[maybe_unused]] bool& removeRequested) {
    // Decided per component type at compile time: the transform is what makes an object an object,
    // so its header has no close button or menu; every other component can be removed.
    if constexpr (std::is_same_v<T, Frost::TransformComponent>) {
        if (!ImGui::CollapsingHeader(T::kDisplayName, ImGuiTreeNodeFlags_DefaultOpen)) { return false; }
    }
    else {
        bool keep = true;
        const bool open = ImGui::CollapsingHeader(T::kDisplayName, &keep, ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove Component")) { keep = false; }
            ImGui::EndPopup();
        }

        if (!keep) {
            removeRequested = true;
            return false;
        }
        if (!open) { return false; }
    }

    return DrawComponent(context, object, object.GetComponent<T>());
}

void InspectorPanel::Draw(EditorContext& context) {
    if (!ImGui::Begin(kWindowName)) {
        ImGui::End();
        return;
    }

    Frost::GameObject* const object = context.GetSelectedObject();

    // An animation preview belongs to the object shown (and stops when it goes or is deselected).
    if (object == nullptr || object->GetId() != m_PlayingObject || !object->HasComponent<Frost::SkeletalMeshComponent>()) {
        m_PlayingObject = 0;
    }
    // A one-shot animation previewed on an object is left to finish, but not held paused once the
    // object is no longer shown.
    if (m_OneShotObject != 0 && (object == nullptr || object->GetId() != m_OneShotObject)) {
        if (Frost::GameObject* const previous = context.GetLevel().FindObject(m_OneShotObject)) {
            if (Frost::SkeletalMeshComponent* const mesh = previous->TryGetComponent<Frost::SkeletalMeshComponent>();
                mesh != nullptr && mesh->IsOneShotPaused()) {
                mesh->StopOnce();
            }
        }
        m_OneShotObject = 0;
    }

    if (object == nullptr) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextDisabled("Click an object in the viewport or the Level list.");
        ImGui::End();
        return;
    }

    bool changed = false;

    std::string name = object->GetName();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (EditorUI::InputText("##Name", name)) {
        object->SetName(name);
        changed = true;
    }

    if (context.IsSimulating()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("Simulating: objects go back to where they were when the simulation stops.");
        ImGui::PopStyleColor();
    }

    // One section per component. A removal waits until every section is drawn, so the object's set
    // of components does not change while the panel is still going through it.
    std::function<void()> removeComponent;
    Frost::BuiltinComponents::ForEach([&]<typename T>() {
        if (!object->HasComponent<T>()) { return; }

        ImGui::PushID(T::kDisplayName);
        bool removeRequested = false;
        changed |= DrawComponentSection<T>(context, *object, removeRequested);
        if constexpr (std::is_same_v<T, Frost::AttachmentComponent>) {
            // Removing the attachment detaches the object where it is.
            if (removeRequested) { m_DetachRequest = object->GetId(); }
        }
        else if constexpr (!std::is_same_v<T, Frost::TransformComponent>) {
            if (removeRequested) { removeComponent = [object] { object->RemoveComponent<T>(); }; }
        }
        ImGui::PopID();
    });

    if (removeComponent) {
        removeComponent();
        changed = true;
    }
    if (m_DetachRequest != 0) {
        context.DetachObject(std::exchange(m_DetachRequest, 0));
        changed = true;
    }
    if (m_AddDefaultAttachmentsRequest != 0) {
        static_cast<void>(context.AddDefaultAttachments(std::exchange(m_AddDefaultAttachmentsRequest, 0)));
        changed = true;
    }

    ImGui::Spacing();
    changed |= DrawAddComponentMenu(*object);

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Focus")) { context.RequestFocus(object->GetId()); }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) { context.DuplicateSelected(); }
    ImGui::SameLine();

    const Frost::ObjectId id = object->GetId();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.1f, 1.0f));
    const bool deleteClicked = ImGui::Button("Delete");
    ImGui::PopStyleColor();

    if (changed) { context.MarkLevelDirty(); }

    // Last: `object` is gone afterwards.
    if (deleteClicked) { context.DeleteObject(id); }

    ImGui::End();
}

bool InspectorPanel::DrawAddComponentMenu(Frost::GameObject& object) {
    bool added = false;

    const float width = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add Component", ImVec2(width, 0.0f))) { ImGui::OpenPopup("AddComponent"); }

    if (ImGui::BeginPopup("AddComponent")) {
        bool any = false;

        Frost::BuiltinComponents::ForEach([&]<typename T>() {
            if (object.HasComponent<T>()) { return; }
            any = true;

            if constexpr (std::is_same_v<T, Frost::LightComponent>) {
                // A light is created as a particular type, with that type's defaults.
                if (ImGui::BeginMenu(T::kDisplayName)) {
                    for (const Frost::LightType type : {Frost::LightType::Directional, Frost::LightType::Point, Frost::LightType::Spot}) {
                        if (ImGui::MenuItem(EditorUI::LightDisplayName(type))) {
                            object.AddComponent<Frost::LightComponent>(Frost::LightComponent::CreateDefault(type));
                            added = true;
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            else if (ImGui::MenuItem(T::kDisplayName)) {
                object.AddComponent<T>(MakeNewComponent<T>(object));
                added = true;
            }
        });

        if (!any) { ImGui::TextDisabled("It has every component."); }
        ImGui::EndPopup();
    }

    return added;
}

bool InspectorPanel::DrawComponent(EditorContext& /*context*/, Frost::GameObject& /*object*/,
                                   Frost::TransformComponent& transform) {
    bool changed = false;

    changed |= ImGui::DragFloat3("Position", &transform.position.x, 5.0f * Frost::kCentimeter, 0.0f, 0.0f, "%.1f cm");
    changed |= ImGui::DragFloat3("Rotation", &transform.rotation.x, 0.5f, 0.0f, 0.0f, "%.1f deg");

    glm::vec3 scale = transform.scale;
    if (ImGui::DragFloat3("Scale", &scale.x, 0.01f, kMinScale, 1.0e4f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
        if (m_UniformScale) {
            // Scale all three by the ratio of whichever component was edited.
            for (int axis = 0; axis < 3; ++axis) {
                if (scale[axis] != transform.scale[axis] && transform.scale[axis] != 0.0f) {
                    const float ratio = scale[axis] / transform.scale[axis];
                    scale = glm::max(transform.scale * ratio, glm::vec3(kMinScale));
                    break;
                }
            }
        }
        transform.scale = scale;
        changed = true;
    }

    ImGui::Checkbox("Uniform scale", &m_UniformScale);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        transform = Frost::TransformComponent{.position = transform.position};
        changed = true;
    }
    ImGui::SetItemTooltip("Reset rotation and scale (keeps the position).");

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::AttachmentComponent& attachment) {
    bool changed = false;

    Frost::Level& level = context.GetLevel();
    const Frost::Registry& registry = level.GetRegistry();
    const Frost::GameObject* const parent = attachment.parent != 0 ? level.FindObject(attachment.parent) : nullptr;

    // The object it is attached to: picking one puts it on its mount; "(none)" detaches it where it is.
    const std::string parentLabel = parent != nullptr          ? parent->GetName()
                                    : attachment.parent != 0 ? "(missing object)"
                                                             : "(none)";
    if (ImGui::BeginCombo("Parent", parentLabel.c_str())) {
        if (ImGui::Selectable("(none)", attachment.parent == 0) && attachment.parent != 0) { m_DetachRequest = object.GetId(); }
        for (const std::unique_ptr<Frost::GameObject>& candidate : level.GetObjects()) {
            if (candidate->GetId() == object.GetId()) { continue; }

            // Not onto something attached to it: that would be a loop.
            const bool allowed = Frost::CanAttach(registry, object.GetId(), candidate->GetId());
            ImGui::PushID(static_cast<int>(candidate->GetId()));
            ImGui::BeginDisabled(!allowed);
            const std::string name = candidate->GetName().empty() ? "(unnamed)" : candidate->GetName();
            if (ImGui::Selectable(name.c_str(), attachment.parent == candidate->GetId()) && attachment.parent != candidate->GetId()) {
                attachment.parent = candidate->GetId();
                attachment.bone.clear();  // A bone of the previous parent's skeleton.
                object.GetTransform() = Frost::TransformComponent{};
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The object this one is attached to and moves with. Picking one puts this object on its mount:\n"
                          "its transform is reset, so a part modeled in the parent's space (with the same origin, as\n"
                          "a weapon's attachments usually are) sits where it was modeled. (none) detaches it, leaving\n"
                          "it where it is.");

    if (attachment.parent != 0 && parent == nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("The object it was attached to is no longer in the level; it stays where it is, unattached.");
        ImGui::PopStyleColor();
    }

    // What it is mounted on: the parent itself, a bone of its skeletal mesh, or a socket of it.
    const Frost::SkeletalMeshComponent* const parentMesh =
            parent != nullptr ? parent->TryGetComponent<Frost::SkeletalMeshComponent>() : nullptr;
    const Frost::SkeletalMeshAsset* const parentModel = parentMesh != nullptr ? parentMesh->GetSkeletalMesh().get() : nullptr;
    const Frost::SkeletonAsset* const skeleton = parentModel != nullptr ? parentModel->skeleton.get() : nullptr;
    const bool onSocket = !attachment.socket.empty();
    const bool mountMissing = onSocket ? parentModel == nullptr || parentModel->FindSocket(attachment.socket) == nullptr
                                       : !attachment.bone.empty() &&
                                                 (skeleton == nullptr || !skeleton->skeleton.FindBone(attachment.bone).has_value());

    std::string mountLabel = onSocket ? "socket " + attachment.socket
                                      : attachment.bone.empty() ? std::string("(the object itself)") : attachment.bone;
    if (mountMissing) {
        mountLabel = "(missing) " + mountLabel;
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    }
    ImGui::BeginDisabled(parent == nullptr);
    const bool mountOpen = ImGui::BeginCombo("Bone", mountLabel.c_str(), ImGuiComboFlags_HeightLarge);
    if (mountMissing) { ImGui::PopStyleColor(); }
    if (mountOpen) {
        if (ImGui::Selectable("(the object itself)", attachment.bone.empty() && !onSocket)) {
            attachment.bone.clear();
            attachment.socket.clear();
            changed = true;
        }
        if (skeleton != nullptr) {
            ImGui::SeparatorText("Bones");
            for (const Frost::Bone& bone : skeleton->skeleton.bones) {
                if (ImGui::Selectable(bone.name.c_str(), !onSocket && bone.name == attachment.bone)) {
                    attachment.bone = bone.name;
                    attachment.socket.clear();
                    changed = true;
                }
            }
        }
        else {
            ImGui::TextDisabled("The parent has no skeletal mesh (with a skeleton).");
        }
        if (parentModel != nullptr && !parentModel->sockets.empty()) {
            ImGui::SeparatorText("Sockets");
            for (const Frost::SocketData& socket : parentModel->sockets) {
                ImGui::PushID(socket.name.c_str());
                if (ImGui::Selectable(socket.name.c_str(), socket.name == attachment.socket)) {
                    attachment.socket = socket.name;
                    attachment.bone.clear();
                    object.GetTransform() = Frost::TransformComponent{};  // At the socket.
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(mountMissing ? "The parent's skeletal mesh has no bone or socket of this name: this object moves with the\n"
                                         "parent itself."
                          : onSocket   ? "The socket of the parent's skeletal mesh this object sits at; its transform is relative to\n"
                                         "the socket (sockets are added in the skeletal mesh's editor)."
                                       : "The bone of the parent's skeletal mesh this object moves with (relative to the bone's\n"
                                         "rest pose, so it stays on its mount as the bone moves), or a socket of it.");

    // Its role, by which animations show and hide it.
    bool committed = false;
    changed |= SlotField("Slot", attachment.slot, KnownSlots(registry, attachment.parent), committed);
    ImGui::SetItemTooltip("The role it fills on the parent (\"Magazine\", \"Muzzle\", ...). The visibility keys of the\n"
                          "parent's own animation (Skeletal Mesh section) show and hide attachments by slot. Animation\n"
                          "triggers name the objects they show and hide instead.");

    // What the parent's animation says about it right now.
    std::optional<bool> animated;
    if (const Frost::MeshComponent* const mesh = object.TryGetComponent<Frost::MeshComponent>()) { animated = mesh->GetAnimationVisibility(); }
    if (const Frost::SkeletalMeshComponent* const mesh = object.TryGetComponent<Frost::SkeletalMeshComponent>()) {
        animated = animated.has_value() ? animated : mesh->GetAnimationVisibility();
    }
    if (animated.has_value()) {
        ImGui::TextDisabled(*animated ? "Shown by the parent's animation at this moment." : "Hidden by the parent's animation at this moment.");
    }

    if (ImGui::SmallButton("Reset to Mount")) {
        object.GetTransform() = Frost::TransformComponent{};
        changed = true;
    }
    ImGui::SetItemTooltip("Put it back exactly where it was modeled relative to the parent (reset its transform).");
    ImGui::SameLine();
    ImGui::BeginDisabled(attachment.parent == 0);
    if (ImGui::SmallButton("Detach")) { m_DetachRequest = object.GetId(); }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Stop moving with the parent, staying where it is now.");

    if (object.HasComponent<Frost::RigidBodyComponent>() || object.HasComponent<Frost::BoxColliderComponent>() ||
        object.HasComponent<Frost::CapsuleColliderComponent>()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("Attached objects are left out of physics: its rigid body and colliders do nothing while it is attached.");
        ImGui::PopStyleColor();
    }

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::AnimationTriggerComponent& component) {
    bool changed = false;

    Frost::SkeletalMeshComponent* const mesh = object.TryGetComponent<Frost::SkeletalMeshComponent>();
    if (mesh == nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("Needs a Skeletal Mesh on this object to play the animations on.");
        ImGui::PopStyleColor();
    }

    std::optional<std::size_t> toRemove;
    for (std::size_t i = 0; i < component.triggers.size(); ++i) {
        Frost::AnimationTrigger& trigger = component.triggers[i];
        ImGui::PushID(static_cast<int>(i));

        // The key.
        const std::string keyName(Frost::GetKeyName(trigger.key));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::BeginCombo("##Key", keyName.empty() ? "?" : keyName.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (const Frost::Key key : Frost::GetNamedKeys()) {
                const std::string name(Frost::GetKeyName(key));
                if (ImGui::Selectable(name.c_str(), key == trigger.key)) {
                    trigger.key = key;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The key that plays the animation in the game.");

        // The animation: a button-shaped drop target (click for the library's animations).
        ImGui::SameLine();
        const std::optional<std::filesystem::path> current =
                trigger.animationReference.IsSet() ? CurrentPath(context, trigger.animationReference) : std::nullopt;
        const bool missing = trigger.animationReference.IsSet() && (trigger.animation == nullptr || !current.has_value());
        std::string label = "(no animation)";
        if (current.has_value()) { label = Frost::PathToUtf8(current->stem()); }
        else if (trigger.animationReference.IsSet()) { label = Frost::PathToUtf8(trigger.animationReference.path.stem()); }
        if (missing) { label = "(missing) " + label; }
        if (trigger.animation != nullptr) { label += "  (" + FormatSeconds(trigger.animation->clip.duration) + ")"; }

        if (missing) { ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor); }
        const bool openMenu = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x, 0.0f));
        if (missing) { ImGui::PopStyleColor(); }
        ImGui::SetItemTooltip("The animation the key plays once. Drop an animation from the Asset Library here, or click to pick one.");

        const auto assign = [&](const std::filesystem::path& path) {
            try {
                trigger.animation = context.GetAssets().LoadAnimation(path);
                trigger.animationReference = Frost::AssetReference{.id = trigger.animation->id, .path = trigger.animation->path};
                changed = true;
            }
            catch (const Frost::ImportError& e) {
                context.ReportError(std::string("Could not load the animation: ") + e.what());
            }
        };
        if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetDrop(Frost::AssetType::Animation)) {
            if (!context.GetLibrary().IsInside(*dropped)) {
                context.ReportError("'" + Frost::PathToUtf8(dropped->filename()) + "' is not part of this project. A project "
                                    "only uses its own assets: copy the file into the project's content folder first.");
            }
            else {
                assign(*dropped);
            }
        }
        if (openMenu) { ImGui::OpenPopup("TriggerAnimation"); }
        if (ImGui::BeginPopup("TriggerAnimation")) {
            bool any = false;
            for (const AssetLibrary::Entry& entry : context.GetLibrary().GetEntries()) {
                if (entry.type != Frost::AssetType::Animation || entry.duplicate) { continue; }
                any = true;
                const std::string name = Frost::PathToUtf8(entry.path.stem());
                if (ImGui::MenuItem(name.c_str(), nullptr, trigger.animationReference.id == entry.id)) { assign(entry.path); }
            }
            if (!any) { ImGui::TextDisabled("No animations imported yet."); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) { toRemove = i; }
        ImGui::SetItemTooltip("Remove this trigger.");

        // Previewing it here: played through, or posed at a moment of it.
        if (trigger.animation != nullptr && mesh != nullptr) {
            const bool active = component.GetPlaying(mesh) == &trigger;
            const bool playing = active && !mesh->IsOneShotPaused();
            if (ImGui::Button(playing ? "Stop" : "Preview", ImVec2(ImGui::GetFontSize() * 4.5f, 0.0f))) {
                if (playing) { mesh->StopOnce(); }
                else {
                    mesh->PlayOnce(trigger.animation);
                    component.playing = i;
                }
                m_OneShotObject = object.GetId();
            }
            ImGui::SetItemTooltip("Play it once here, as the key does in the game.");

            ImGui::SameLine();
            const float duration = trigger.animation->clip.duration;
            float time = active ? std::min(mesh->GetOneShotTime(), duration) : 0.0f;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##Moment", &time, 0.0f, duration, "%.2f s", ImGuiSliderFlags_AlwaysClamp)) {
                mesh->PlayOnce(trigger.animation, time);
                mesh->SetOneShotPaused(true);
                component.playing = i;
                m_OneShotObject = object.GetId();
            }
            ImGui::SetItemTooltip("Pose the model at this moment of the animation (for placing visibility keys). Not saved:\n"
                                  "the pose goes back when another object is selected.");

            const std::string keysLabel = "Visibility keys (" + std::to_string(trigger.visibilityKeys.size()) + ")###Keys";
            if (ImGui::TreeNodeEx(keysLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= DrawTriggerKeys(context, object, trigger, time);
                ImGui::TreePop();
            }
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    if (toRemove.has_value()) {
        component.triggers.erase(component.triggers.begin() + static_cast<std::ptrdiff_t>(*toRemove));
        if (mesh != nullptr && component.GetPlaying(mesh) != nullptr) { mesh->StopOnce(); }
        component.playing.reset();  // (Indices moved.)
        changed = true;
    }

    if (ImGui::Button("Add Trigger")) {
        // R, or the first key no trigger uses yet.
        Frost::Key key = Frost::Key::R;
        const auto used = [&](const Frost::Key candidate) {
            return std::any_of(component.triggers.begin(), component.triggers.end(),
                               [candidate](const Frost::AnimationTrigger& trigger) { return trigger.key == candidate; });
        };
        if (used(key)) {
            for (const Frost::Key candidate : Frost::GetNamedKeys()) {
                if (!used(candidate)) {
                    key = candidate;
                    break;
                }
            }
        }
        component.triggers.push_back(Frost::AnimationTrigger{.key = key, .animationReference = {}, .animation = nullptr, .visibilityKeys = {}});
        changed = true;
    }
    ImGui::SetItemTooltip("A key that plays an animation once on the skeletal mesh in the game (a reload on R, say).\n"
                          "While one is playing, the keys are ignored.");

    return changed;
}

bool InspectorPanel::DrawTriggerKeys(EditorContext& context, const Frost::GameObject& object, Frost::AnimationTrigger& trigger,
                                     const float time) {
    bool changed = false;
    Frost::Level& level = context.GetLevel();
    const float duration = trigger.animation != nullptr ? trigger.animation->clip.duration : 0.0f;

    // The objects it can show and hide: those attached to this one (and to those, and so on).
    std::vector<Frost::EntityId> targets = Frost::CollectSubtree(level.GetRegistry(), object.GetId());
    targets.erase(targets.begin());  // (This object itself.)
    const auto nameOf = [&](const Frost::EntityId id) -> std::string {
        const Frost::GameObject* const target = level.FindObject(id);
        if (target == nullptr) { return "(missing object)"; }
        std::string name = target->GetName().empty() ? "(unnamed)" : target->GetName();
        // Objects of the same name (copies of a part) are told apart by id.
        const bool shared = std::any_of(targets.begin(), targets.end(), [&](const Frost::EntityId other) {
            const Frost::GameObject* const candidate = level.FindObject(other);
            return other != id && candidate != nullptr && candidate->GetName() == target->GetName();
        });
        if (shared) { name += "  #" + std::to_string(id); }
        return name;
    };

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Which of the objects attached to this one the animation shows or hides, and from when.");
    ImGui::PopStyleColor();
    std::optional<std::size_t> toRemove;
    bool sort = false;
    if (!trigger.visibilityKeys.empty() &&
        ImGui::BeginTable("##TriggerKeys", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Shows", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##Remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();

        for (std::size_t k = 0; k < trigger.visibilityKeys.size(); ++k) {
            Frost::TriggerVisibilityKey& key = trigger.visibilityKeys[k];
            ImGui::PushID(static_cast<int>(k));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat("##Time", &key.time, 0.01f, 0.0f, duration, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
            sort |= ImGui::IsItemDeactivatedAfterEdit();  // Reordered once the edit is done, not under the cursor.

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool missing = level.FindObject(key.object) == nullptr;
            if (missing) { ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor); }
            const bool open = ImGui::BeginCombo("##Object", nameOf(key.object).c_str(), ImGuiComboFlags_HeightLarge);
            if (missing) { ImGui::PopStyleColor(); }
            if (open) {
                if (targets.empty()) { ImGui::TextDisabled("Nothing is attached to this object."); }
                for (const Frost::EntityId target : targets) {
                    ImGui::PushID(static_cast<int>(target));
                    if (ImGui::Selectable(nameOf(target).c_str(), target == key.object)) {
                        key.object = target;
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##Visible", key.visible ? "Show" : "Hide")) {
                if (ImGui::Selectable("Show", key.visible)) {
                    key.visible = true;
                    changed = true;
                }
                if (ImGui::Selectable("Hide", !key.visible)) {
                    key.visible = false;
                    changed = true;
                }
                ImGui::EndCombo();
            }

            ImGui::TableNextColumn();
            if (ImGui::Button("x", ImVec2(-FLT_MIN, 0.0f))) { toRemove = k; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (toRemove.has_value()) {
        trigger.visibilityKeys.erase(trigger.visibilityKeys.begin() + static_cast<std::ptrdiff_t>(*toRemove));
        changed = true;
    }

    const float at = std::min(time, duration);
    ImGui::BeginDisabled(targets.empty());
    const std::string add = "Add Key at " + FormatSeconds(at);
    if (ImGui::SmallButton(add.c_str())) {
        // Continuing from the last key: the same object, the other way.
        Frost::TriggerVisibilityKey key{.time = at, .object = targets.front(), .visible = false};
        if (!trigger.visibilityKeys.empty()) {
            key.object = trigger.visibilityKeys.back().object;
            key.visible = !trigger.visibilityKeys.back().visible;
        }
        trigger.visibilityKeys.push_back(key);
        sort = true;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(targets.empty() ? "Attach objects to this one first (drag them onto it in the Level list)."
                                          : "A key at the moment the model is posed at: from then on, while the animation plays,\n"
                                            "the object is shown (or hidden). Before its first key, and once the animation is over,\n"
                                            "it is as set on the object (Visible).");

    if (sort) { trigger.SortVisibilityKeys(); }
    return changed;
}

void InspectorPanel::DrawVisibilityKeys(EditorContext& context, const Frost::GameObject& object,
                                        const std::shared_ptr<const Frost::AnimationAsset>& animation, const float time) {
    // The keys as being edited: the asset's, unless an edit of them is under way.
    KeyDraft& draft = m_KeyDrafts[animation->id];
    if (draft.source.lock() != animation) {
        draft.source = animation;
        draft.keys = animation->clip.visibilityKeys;
    }

    bool commit = false;
    const std::vector<std::string> slots = KnownSlots(context.GetLevel().GetRegistry(), object.GetId());

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("When the animation shows or hides the attachments in a slot (as this object's own animation; "
                       "animation triggers name objects instead). Saved to the animation asset.");
    ImGui::PopStyleColor();
    std::optional<std::size_t> toRemove;
    if (!draft.keys.empty() &&
        ImGui::BeginTable("##VisibilityKeys", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Shows", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##Remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < draft.keys.size(); ++i) {
            Frost::VisibilityKey& key = draft.keys[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##Time", &key.time, 0.01f, 0.0f, animation->clip.duration, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
            commit |= ImGui::IsItemDeactivatedAfterEdit();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            SlotField("##Slot", key.slot, slots, commit);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##Visible", key.visible ? "Show" : "Hide")) {
                if (ImGui::Selectable("Show", key.visible)) {
                    key.visible = true;
                    commit = true;
                }
                if (ImGui::Selectable("Hide", !key.visible)) {
                    key.visible = false;
                    commit = true;
                }
                ImGui::EndCombo();
            }

            ImGui::TableNextColumn();
            if (ImGui::Button("x", ImVec2(-FLT_MIN, 0.0f))) { toRemove = i; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (toRemove.has_value()) {
        draft.keys.erase(draft.keys.begin() + static_cast<std::ptrdiff_t>(*toRemove));
        commit = true;
    }

    const std::string add = "Add Key at " + FormatSeconds(std::min(time, animation->clip.duration));
    if (ImGui::SmallButton(add.c_str())) {
        // Continuing from the last key: the same slot, the other way.
        Frost::VisibilityKey key{.time = std::min(time, animation->clip.duration), .slot = {}, .visible = false};
        if (!draft.keys.empty()) {
            key.slot = draft.keys.back().slot;
            key.visible = !draft.keys.back().visible;
        }
        else if (!slots.empty()) {
            key.slot = slots.front();
        }
        draft.keys.push_back(std::move(key));
        commit = true;
    }
    ImGui::SetItemTooltip("A key at the moment the model is posed at: from then on, the slot's attachments are shown\n"
                          "(or hidden) while this animation plays. Before a slot's first key, the animation leaves it\n"
                          "as it is set on the object (Visible).");

    if (commit) {
        // A key without a slot would not be saved (nor do anything): keep it until it has one.
        if (std::any_of(draft.keys.begin(), draft.keys.end(), [](const Frost::VisibilityKey& key) { return key.slot.empty(); })) {
            return;
        }
        const Frost::AssetReference reference{.id = animation->id, .path = animation->path};
        if (const std::string error = context.SetVisibilityKeys(reference, draft.keys); !error.empty()) {
            context.ReportError(error);
            draft.source.reset();  // Start over from the asset.
        }
    }
}

void InspectorPanel::DrawAttachedObjects(EditorContext& context, const Frost::GameObject& object,
                                         const Frost::SkeletalMeshComponent& mesh) {
    Frost::Level& level = context.GetLevel();
    const std::vector<Frost::EntityId> children = Frost::GetAttachedObjects(level.GetRegistry(), object.GetId());
    const std::size_t defaults = mesh.GetSkeletalMesh() != nullptr ? mesh.GetSkeletalMesh()->defaultAttachments.size() : 0;
    if (children.empty() && defaults == 0) { return; }

    ImGui::SeparatorText("Attachments");
    for (const Frost::EntityId id : children) {
        const Frost::GameObject* const child = level.FindObject(id);
        const Frost::AttachmentComponent* const attachment =
                child != nullptr ? child->TryGetComponent<Frost::AttachmentComponent>() : nullptr;
        if (attachment == nullptr) { continue; }

        ImGui::PushID(static_cast<int>(id));
        const std::string name = child->GetName().empty() ? "(unnamed)" : child->GetName();
        if (ImGui::SmallButton(name.c_str())) { context.Select(id); }
        ImGui::SetItemTooltip("Select it.");
        ImGui::SameLine();
        const std::string mount = !attachment->socket.empty() ? "socket " + attachment->socket
                                  : attachment->bone.empty()    ? std::string("(the object itself)")
                                                                : attachment->bone;
        ImGui::TextDisabled("%s%s%s", mount.c_str(), attachment->slot.empty() ? "" : "  slot ", attachment->slot.c_str());
        ImGui::PopID();
    }
    if (children.empty()) { ImGui::TextDisabled("Nothing attached."); }

    if (defaults > 0) {
        if (ImGui::SmallButton("Add Default Attachments")) { m_AddDefaultAttachmentsRequest = object.GetId(); }
        ImGui::SetItemTooltip("Attach the %zu part(s) the model was imported with as separate objects (its magazine,\n"
                              "muzzle device, ...), as when it is placed. Delete the ones you do not want, or replace their\n"
                              "meshes.",
                              defaults);
    }
    ImGui::TextDisabled("Drag objects onto this one in the Level list to attach them.");
}

bool InspectorPanel::DrawComponent(EditorContext& /*context*/, Frost::GameObject& object, Frost::LightComponent& light) {
    bool changed = false;

    // Type. Switching keeps the shared settings (color, intensity, ...); the type-specific ones
    // simply start being used.
    constexpr Frost::LightType kTypes[] = {Frost::LightType::Directional, Frost::LightType::Point, Frost::LightType::Spot};
    if (ImGui::BeginCombo("Type", EditorUI::LightDisplayName(light.type))) {
        for (const Frost::LightType type : kTypes) {
            if (ImGui::Selectable(EditorUI::LightDisplayName(type), type == light.type)) {
                light.type = type;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    changed |= ImGui::Checkbox("Enabled", &light.enabled);

    // Color is stored linear; the picker shows (and edits) its sRGB encoding.
    glm::vec3 color = EditorUI::LinearToSrgb(light.color);
    if (ImGui::ColorEdit3("Color", &color.x)) {
        light.color = EditorUI::SrgbToLinear(color);
        changed = true;
    }

    changed |= ImGui::Checkbox("Use temperature", &light.useTemperature);
    ImGui::SetItemTooltip("Tint the color by a black-body color temperature (warm ~2700 K, daylight ~6500 K).");
    if (light.useTemperature) {
        changed |= ImGui::SliderFloat("Temperature", &light.temperature, 1000.0f, 15000.0f, "%.0f K",
                                      ImGuiSliderFlags_AlwaysClamp);

        // What the temperature tint looks like.
        const glm::vec3 swatch = EditorUI::LinearToSrgb(Frost::ColorTemperatureToLinearRgb(light.temperature));
        ImGui::SameLine();
        ImGui::ColorButton("##TemperatureSwatch", ImVec4(swatch.x, swatch.y, swatch.z, 1.0f), ImGuiColorEditFlags_NoTooltip);
    }

    // Brightness. Drag speed scales with the value so both 0.5 and 5000 are comfortable to edit.
    const float intensitySpeed = std::max(0.01f, light.intensity * 0.01f);
    changed |= ImGui::DragFloat("Intensity", &light.intensity, intensitySpeed, 0.0f, 1.0e6f, "%.2f",
                                ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip(light.type == Frost::LightType::Directional
                                  ? "Illuminance on a surface facing the light (3 is about full white)."
                                  : "Brightness at 1 m away; falls off with the square of the distance.");

    if (light.type != Frost::LightType::Directional) {
        const char* const rangeLabel = light.type == Frost::LightType::Point ? "Radius" : "Distance";
        changed |= ImGui::DragFloat(rangeLabel, &light.range, std::max(1.0f * Frost::kCentimeter, light.range * 0.01f),
                                    1.0f * Frost::kCentimeter, 100.0f * Frost::kKilometer, "%.1f cm",
                                    ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("How far the light reaches; it fades smoothly to nothing at this distance.");
    }

    if (light.type == Frost::LightType::Spot) {
        // Half-angles; the inner cone is kept inside the outer one whichever is edited.
        if (ImGui::SliderFloat("Outer angle", &light.outerConeAngle, 1.0f, 89.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp)) {
            light.innerConeAngle = std::min(light.innerConeAngle, light.outerConeAngle);
            changed = true;
        }
        ImGui::SetItemTooltip("Half-angle of the cone; no light outside it.");

        if (ImGui::SliderFloat("Inner angle", &light.innerConeAngle, 0.0f, 89.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp)) {
            light.outerConeAngle = std::max(light.outerConeAngle, light.innerConeAngle);
            changed = true;
        }
        ImGui::SetItemTooltip("Half-angle of the fully lit core; the light fades from here to the outer angle.");
    }

    if (light.type != Frost::LightType::Point) {
        const glm::vec3 direction = object.GetTransform().GetForward();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Direction %.2f, %.2f, %.2f (rotate the object to aim it)", direction.x, direction.y, direction.z);
        ImGui::PopStyleColor();
    }

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& context, Frost::GameObject& /*object*/, Frost::MeshComponent& mesh) {
    bool changed = false;

    const Frost::AssetReference& reference = mesh.GetMeshReference();
    const std::optional<std::filesystem::path> current = reference.IsSet() ? CurrentPath(context, reference) : std::nullopt;
    const bool missing = reference.IsSet() && (mesh.GetMesh() == nullptr || !current.has_value());

    // The asset's current name (it may have been renamed since the level was loaded).
    std::string label = "(none)";
    if (current.has_value()) { label = Frost::PathToUtf8(current->stem()); }
    else if (reference.IsSet()) { label = Frost::PathToUtf8(reference.path.stem()); }
    if (missing) { label = "(missing) " + label; }

    // A button-shaped drop target: drop a model here to replace this one.
    if (missing) { ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor); }
    ImGui::Button(label.c_str(), ImVec2(-FLT_MIN - 60.0f, 0.0f));
    if (missing) { ImGui::PopStyleColor(); }

    if (current.has_value()) { ImGui::SetItemTooltip("%s", Frost::PathToUtf8(*current).c_str()); }
    else if (reference.IsSet()) {
        ImGui::SetItemTooltip("Not found: %s\nIt may have been deleted. Moving or restoring the file brings it back.",
                              Frost::PathToUtf8(reference.path).c_str());
    }
    else { ImGui::SetItemTooltip("Drop a model from the Asset Library here."); }

    if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetDrop(Frost::AssetType::Mesh)) {
        try {
            mesh.SetMesh(context.GetAssets().LoadMesh(*dropped));
            changed = true;
        }
        catch (const Frost::ImportError& e) {
            context.ReportError(std::string("Could not load the model: ") + e.what());
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!reference.IsSet());
    if (ImGui::Button("Clear", ImVec2(-FLT_MIN, 0.0f))) {
        mesh.SetMesh(nullptr);
        changed = true;
    }
    ImGui::EndDisabled();

    if (mesh.GetMesh() != nullptr) {
        const Frost::Mesh& model = *mesh.GetMesh()->mesh;
        ImGui::TextDisabled("%zu part(s), %u vertices, %u triangles", model.GetSubMeshes().size(), model.GetVertexCount(),
                            model.GetIndexCount() / 3);
    }

    changed |= DrawVisibility(mesh);

    ImGui::SeparatorText("Materials");
    changed |= DrawMaterials(context, mesh, mesh.GetMesh().get());

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::SkeletalMeshComponent& mesh) {
    bool changed = false;

    const Frost::AssetReference& reference = mesh.GetSkeletalMeshReference();
    const std::optional<std::filesystem::path> current = reference.IsSet() ? CurrentPath(context, reference) : std::nullopt;
    const bool missing = reference.IsSet() && (mesh.GetSkeletalMesh() == nullptr || !current.has_value());

    std::string label = "(none)";
    if (current.has_value()) { label = Frost::PathToUtf8(current->stem()); }
    else if (reference.IsSet()) { label = Frost::PathToUtf8(reference.path.stem()); }
    if (missing) { label = "(missing) " + label; }

    // A button-shaped drop target: drop a skeletal mesh here to replace this one.
    if (missing) { ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor); }
    ImGui::Button(label.c_str(), ImVec2(-FLT_MIN - 60.0f, 0.0f));
    if (missing) { ImGui::PopStyleColor(); }

    if (current.has_value()) { ImGui::SetItemTooltip("%s", Frost::PathToUtf8(*current).c_str()); }
    else if (reference.IsSet()) {
        ImGui::SetItemTooltip("Not found: %s\nIt may have been deleted. Moving or restoring the file brings it back.",
                              Frost::PathToUtf8(reference.path).c_str());
    }
    else { ImGui::SetItemTooltip("Drop a skeletal mesh from the Asset Library here."); }

    if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetDrop(Frost::AssetType::SkeletalMesh)) {
        try {
            mesh.SetSkeletalMesh(context.GetAssets().LoadSkeletalMesh(*dropped));
            changed = true;
        }
        catch (const Frost::ImportError& e) {
            context.ReportError(std::string("Could not load the skeletal mesh: ") + e.what());
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!reference.IsSet());
    if (ImGui::Button("Clear", ImVec2(-FLT_MIN, 0.0f))) {
        mesh.SetSkeletalMesh(nullptr);
        changed = true;
    }
    ImGui::EndDisabled();

    if (const std::shared_ptr<const Frost::SkeletalMeshAsset>& asset = mesh.GetSkeletalMesh(); asset != nullptr) {
        const Frost::Mesh& model = *asset->drawable->mesh;
        ImGui::TextDisabled("%zu part(s), %u vertices, %u triangles", model.GetSubMeshes().size(), model.GetVertexCount(),
                            model.GetIndexCount() / 3);
        changed |= DrawVisibility(mesh);

        // Its skeleton (by reference: it may have been renamed, moved or deleted since).
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Skeleton");
        ImGui::SameLine();
        if (asset->skeleton != nullptr) {
            const std::optional<std::filesystem::path> skeletonPath = CurrentPath(context, asset->skeletonReference);
            const std::string name = Frost::PathToUtf8(skeletonPath.value_or(asset->skeleton->path).stem());
            ImGui::TextDisabled("%s (%zu bones)", name.c_str(), asset->skeleton->skeleton.bones.size());
            ImGui::SetItemTooltip("%s", Frost::PathToUtf8(skeletonPath.value_or(asset->skeleton->path)).c_str());

            if (asset->missingBones > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
                ImGui::TextWrapped("%d bone(s) the mesh is skinned to are not in this skeleton; what they move stays as "
                                   "modeled. Reimport the mesh with its own skeleton, or one with the same bones.",
                                   asset->missingBones);
                ImGui::PopStyleColor();
            }
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextUnformatted(asset->skeletonReference.IsSet() ? "(missing)" : "(none)");
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("The skeleton this mesh was imported for could not be found (%s), so it is shown as "
                                  "modeled and cannot be posed.",
                                  Frost::PathToUtf8(asset->skeletonReference.path.filename()).c_str());
        }
    }

    // Parts attached to bones (a magazine, a muzzle brake): which follow which bone, and whether
    // they do on this object.
    if (const std::shared_ptr<const Frost::SkeletalMeshAsset>& asset = mesh.GetSkeletalMesh(); asset != nullptr) {
        const std::vector<Frost::SubMesh>& subMeshes = asset->data.mesh.subMeshes;
        bool anyAttached = false;
        for (std::size_t i = 0; i < subMeshes.size() && i < asset->data.attachments.size(); ++i) {
            anyAttached = anyAttached || asset->data.attachments[i] >= 0;
        }

        if (anyAttached) {
            ImGui::SeparatorText("Attached Parts");
            bool follow = mesh.GetAttachmentsFollowBones();
            if (ImGui::Checkbox("Follow bones", &follow)) {
                mesh.SetAttachmentsFollowBones(follow);
                changed = true;
            }
            ImGui::SetItemTooltip("Whether the parts attached to bones move with them. Off, they stay where they were\n"
                                  "modeled while the rest of the model moves (e.g. for parts gameplay code places).\n"
                                  "Which bone a part follows is chosen when the model is imported.");

            for (std::size_t i = 0; i < subMeshes.size() && i < asset->data.attachments.size(); ++i) {
                const std::int32_t attachment = asset->data.attachments[i];
                if (attachment < 0) { continue; }
                const std::string part = subMeshes[i].name.empty() ? "Part " + std::to_string(i) : subMeshes[i].name;
                ImGui::BulletText("%s  ->  %s", part.c_str(), asset->data.bindBones[static_cast<std::size_t>(attachment)].c_str());
            }
        }
    }

    DrawAttachedObjects(context, object, mesh);

    ImGui::SeparatorText("Animation");
    changed |= DrawAnimation(context, object, mesh);

    ImGui::SeparatorText("Materials");
    changed |= DrawMaterials(context, mesh, mesh.GetDrawable().get());

    return changed;
}

bool InspectorPanel::DrawAnimation(EditorContext& context, const Frost::GameObject& object, Frost::SkeletalMeshComponent& mesh) {
    bool changed = false;

    const Frost::AssetReference& reference = mesh.GetAnimationReference();
    const std::shared_ptr<const Frost::AnimationAsset>& animation = mesh.GetAnimation();
    const std::optional<std::filesystem::path> current = reference.IsSet() ? CurrentPath(context, reference) : std::nullopt;
    const bool missing = reference.IsSet() && (animation == nullptr || !current.has_value());

    std::string label = "(rest pose)";
    if (current.has_value()) { label = Frost::PathToUtf8(current->stem()); }
    else if (reference.IsSet()) { label = Frost::PathToUtf8(reference.path.stem()); }
    if (missing) { label = "(missing) " + label; }
    if (animation != nullptr) { label += "  (" + FormatSeconds(animation->clip.duration) + ")"; }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Animation");
    ImGui::SameLine();

    // A button-shaped drop target (click for the library's animations).
    if (missing) { ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor); }
    const bool openMenu = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN - 60.0f, 0.0f));
    if (missing) { ImGui::PopStyleColor(); }

    if (current.has_value()) { ImGui::SetItemTooltip("%s", Frost::PathToUtf8(*current).c_str()); }
    else if (reference.IsSet()) {
        ImGui::SetItemTooltip("Not found: %s\nIt may have been deleted. Moving or restoring the file brings it back.",
                              Frost::PathToUtf8(reference.path).c_str());
    }
    else { ImGui::SetItemTooltip("Drop an animation from the Asset Library here, or click to pick one."); }

    const auto assign = [&](const std::filesystem::path& path) {
        try {
            mesh.SetAnimation(context.GetAssets().LoadAnimation(path));
            mesh.SetAnimationTime(0.0f);
            changed = true;
        }
        catch (const Frost::ImportError& e) {
            context.ReportError(std::string("Could not load the animation: ") + e.what());
        }
    };

    if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetDrop(Frost::AssetType::Animation)) {
        if (!context.GetLibrary().IsInside(*dropped)) {
            context.ReportError("'" + Frost::PathToUtf8(dropped->filename()) + "' is not part of this project. A project "
                                "only uses its own assets: copy the file into the project's content folder first.");
        }
        else {
            assign(*dropped);
        }
    }

    if (openMenu) { ImGui::OpenPopup("AnimationMenu"); }
    if (ImGui::BeginPopup("AnimationMenu")) {
        if (ImGui::MenuItem("Rest pose (no animation)", nullptr, !reference.IsSet())) {
            mesh.SetAnimation(nullptr);
            changed = true;
        }

        ImGui::SeparatorText("Library");
        bool any = false;
        for (const AssetLibrary::Entry& entry : context.GetLibrary().GetEntries()) {
            if (entry.type != Frost::AssetType::Animation || entry.duplicate) { continue; }
            any = true;

            const std::string name = Frost::PathToUtf8(entry.path.stem());
            if (ImGui::MenuItem(name.c_str(), nullptr, reference.id == entry.id)) { assign(entry.path); }
            ImGui::SetItemTooltip("%s", Frost::GenericPathToUtf8(entry.path.lexically_relative(context.GetLibrary().GetContentDirectory())).c_str());
        }
        if (!any) { ImGui::TextDisabled("No animations imported yet."); }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!reference.IsSet());
    if (ImGui::Button("Clear##Animation", ImVec2(-FLT_MIN, 0.0f))) {
        mesh.SetAnimation(nullptr);
        changed = true;
    }
    ImGui::EndDisabled();

    // Stop previewing when the animation goes (or another object is shown).
    const bool canPlay = animation != nullptr && mesh.GetSkeletalMesh() != nullptr &&
                         mesh.GetSkeletalMesh()->skeleton != nullptr && animation->clip.duration > 0.0f;
    if (!canPlay && m_PlayingObject == object.GetId()) {
        m_PlayingObject = 0;
        mesh.Stop();
    }

    if (animation == nullptr) {
        if (mesh.GetSkeletalMesh() != nullptr) { ImGui::TextDisabled("Shown in its skeleton's rest pose."); }
        return changed;
    }

    // Previewing: the animation plays here as it would in the game (looping or not, at its
    // speed); where it stops is the pose the level keeps.
    bool playing = m_PlayingObject == object.GetId();
    if (playing) {
        mesh.Play();
        mesh.Advance(ImGui::GetIO().DeltaTime);
        changed = true;
        if (!mesh.IsPlaying()) {  // Reached the end, not looping.
            m_PlayingObject = 0;
            playing = false;
        }
    }

    ImGui::BeginDisabled(!canPlay);
    if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(ImGui::GetFontSize() * 4.0f, 0.0f))) {
        if (playing) {
            m_PlayingObject = 0;
            mesh.Stop();
        }
        else {
            // From the start again if it had played to the end.
            if (!mesh.IsLooping() && mesh.GetAnimationTime() >= animation->clip.duration) { mesh.SetAnimationTime(0.0f); }
            m_PlayingObject = object.GetId();
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Preview the animation here, as the game plays it. The pose it stops at is the one the level\n"
                          "keeps (and where the game starts playing it).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    float time = std::min(mesh.GetAnimationTime(), animation->clip.duration);
    if (ImGui::SliderFloat("##Time", &time, 0.0f, animation->clip.duration, "%.2f s", ImGuiSliderFlags_AlwaysClamp)) {
        mesh.SetAnimationTime(time);
        m_PlayingObject = 0;
        mesh.Stop();
        changed = true;
    }
    ImGui::SetItemTooltip("The moment of the animation the model is posed at in the editor, and where the game starts\n"
                          "playing it.");

    // How the game plays it.
    bool autoPlay = mesh.GetAutoPlay();
    if (ImGui::Checkbox("Play in game", &autoPlay)) {
        mesh.SetAutoPlay(autoPlay);
        changed = true;
    }
    ImGui::SetItemTooltip("Start playing the animation when the game starts (from the time above).");
    ImGui::SameLine();
    bool looping = mesh.IsLooping();
    if (ImGui::Checkbox("Loop", &looping)) {
        mesh.SetLooping(looping);
        changed = true;
    }
    ImGui::SetItemTooltip("Start over at the end; otherwise it stops there.");
    ImGui::SameLine();
    float rate = mesh.GetPlayRate();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragFloat("##Speed", &rate, 0.01f, -10.0f, 10.0f, "speed x%.2f", ImGuiSliderFlags_AlwaysClamp)) {
        mesh.SetPlayRate(rate);
        changed = true;
    }
    ImGui::SetItemTooltip("How fast it plays (1 = as authored; negative plays it backwards).");

    const std::string keysLabel = "Visibility keys (" + std::to_string(animation->clip.visibilityKeys.size()) + ")###Keys";
    if (ImGui::TreeNode(keysLabel.c_str())) {
        DrawVisibilityKeys(context, object, animation, std::min(mesh.GetAnimationTime(), animation->clip.duration));
        ImGui::TreePop();
    }

    // Animations play by bone name: one made for another rig moves nothing.
    const std::size_t bound = mesh.GetBoundTrackCount();
    const std::size_t tracks = animation->clip.tracks.size();
    if (mesh.GetSkeletalMesh() != nullptr && mesh.GetSkeletalMesh()->skeleton != nullptr) {
        if (bound == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
            ImGui::TextWrapped("This animation moves none of this skeleton's bones: it was made for a skeleton with "
                               "different bone names.");
            ImGui::PopStyleColor();
        }
        else if (bound < tracks) {
            ImGui::TextDisabled("Moves %zu of the %zu bones it animates (the skeleton lacks the others).", bound, tracks);
        }
    }

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::CameraComponent& camera) {
    bool changed = false;

    changed |= ImGui::SliderFloat("Field of view", &camera.verticalFov, 10.0f, 120.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Vertical: from the bottom of the view to the top.");
    changed |= ImGui::DragFloat("Near clip", &camera.nearClip, 0.5f * Frost::kCentimeter, 0.1f * Frost::kCentimeter,
                                100.0f * Frost::kMeter, "%.2f cm", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Nothing closer than this is drawn.");
    changed |= ImGui::DragFloat("Far clip", &camera.farClip, 1.0f * Frost::kMeter, camera.nearClip + 1.0f * Frost::kCentimeter,
                                1000.0f * Frost::kKilometer, "%.0f cm",
                                ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Nothing further than this is drawn.");

    ImGui::SeparatorText("Mount");
    changed |= ImGui::DragFloat3("Offset", &camera.offset.x, 1.0f * Frost::kCentimeter, 0.0f, 0.0f, "%.1f cm");
    ImGui::SetItemTooltip("Where the camera sits on its object, in the object's space (e.g. a character's eye height).");
    changed |= ImGui::DragFloat3("Rotation##Mount", &camera.rotation.x, 0.5f, 0.0f, 0.0f, "%.1f deg");
    ImGui::SetItemTooltip("The camera's own turn on top of its object's (pitch, yaw, roll).");

    if (ImGui::SmallButton("Align with View")) {
        // The object turns left/right to the view's yaw and the camera tilts to its pitch (the way
        // a character looks around), and the object moves so that the camera, at its offset, ends
        // up exactly at the viewport's position.
        const Frost::ViewPoint& view = context.GetView();
        Frost::TransformComponent& transform = object.GetTransform();
        transform.rotation = glm::vec3(0.0f, view.yawDegrees, 0.0f);
        camera.rotation = glm::vec3(view.pitchDegrees, 0.0f, 0.0f);
        transform.position = view.position - transform.GetRotation() * camera.offset;
        changed = true;
    }
    ImGui::SetItemTooltip("Move the camera (with its object) to where the viewport is looking from.");

    ImGui::TextDisabled("The game looks through the level's first camera.");
    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& /*context*/, Frost::GameObject& object,
                                   Frost::FlyControllerComponent& controller) {
    bool changed = false;

    changed |= ImGui::DragFloat("Speed", &controller.moveSpeed, 5.0f * Frost::kCentimeter, 5.0f * Frost::kCentimeter,
                                500.0f * Frost::kMeter, "%.0f cm/s",
                                ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Centimeters per second (the mouse wheel changes it while playing).");
    changed |= ImGui::DragFloat("Shift factor", &controller.fastMultiplier, 0.05f, 1.0f, 50.0f, "x%.2f",
                                ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How much faster it flies while Left Shift is held.");
    changed |= ImGui::DragFloat("Sensitivity", &controller.lookSensitivity, 0.005f, 0.005f, 2.0f, "%.3f deg",
                                ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Mouse sensitivity: degrees of turn per unit of mouse movement.");

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("In the game: click to look around with the mouse, W/A/S/D to move, Space / Left Ctrl "
                       "up and down, Shift faster, Esc gives the mouse back.%s",
                       object.HasComponent<Frost::CameraComponent>() ? "" : " Add a Camera to see through it.");
    ImGui::PopStyleColor();

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& /*context*/, Frost::GameObject& object,
                                   Frost::CharacterControllerComponent& controller) {
    bool changed = false;

    changed |= ImGui::DragFloat("Walk speed", &controller.walkSpeed, 5.0f * Frost::kCentimeter, 0.0f, 100.0f * Frost::kMeter,
                                "%.0f cm/s", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::DragFloat("Jump speed", &controller.jumpSpeed, 5.0f * Frost::kCentimeter, 0.0f, 100.0f * Frost::kMeter,
                                "%.0f cm/s", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Upward speed at take-off: a jump rises about speed^2 / (2 x 981) cm.");
    changed |= ImGui::DragFloat("Sensitivity", &controller.lookSensitivity, 0.005f, 0.005f, 2.0f, "%.3f deg",
                                ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Mouse sensitivity: degrees of turn per unit of mouse movement.");

    // What it needs to work, said where it is set up.
    const Frost::RigidBodyComponent* const body = object.TryGetComponent<Frost::RigidBodyComponent>();
    const bool collides = object.HasComponent<Frost::CapsuleColliderComponent>() ||
                          object.HasComponent<Frost::BoxColliderComponent>();
    const char* problem = nullptr;
    if (body == nullptr || !body->simulated) { problem = "Needs a simulated Rigid Body to walk, fall and jump."; }
    else if (!collides) { problem = "Needs a collider (a Capsule Collider) to stand on the ground."; }
    else if (!object.HasComponent<Frost::CameraComponent>()) {
        problem = "Add a Camera (mounted at head height) to see through it.";
    }
    if (problem != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("%s", problem);
        ImGui::PopStyleColor();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("In the game: click to look around (mouse left/right turns the body, up/down tilts the camera), "
                       "W/A/S/D to walk, Space to jump, Esc gives the mouse back.");
    ImGui::PopStyleColor();

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& /*context*/, Frost::GameObject& object,
                                   Frost::CapsuleColliderComponent& collider) {
    bool changed = false;

    changed |= ImGui::DragFloat3("Center", &collider.center.x, 1.0f * Frost::kCentimeter, 0.0f, 0.0f, "%.1f cm");
    ImGui::SetItemTooltip("The capsule's middle, in the object's own space.");
    changed |= ImGui::DragFloat("Radius", &collider.radius, 0.5f * Frost::kCentimeter, 0.0f, 100.0f * Frost::kMeter, "%.1f cm",
                                ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::DragFloat("Height", &collider.height, 1.0f * Frost::kCentimeter, 0.0f, 100.0f * Frost::kMeter, "%.1f cm",
                                ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Total, tip to tip, along the object's up axis (at most 2 x radius: a sphere).");

    const std::optional<Frost::Bounds> bounds = ModelBounds(object);
    ImGui::BeginDisabled(!bounds.has_value());
    if (ImGui::SmallButton("Fit to Model")) {
        collider = Frost::CapsuleColliderComponent::FromBounds(*bounds);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(bounds.has_value() ? "Stand the capsule in the object's model's box."
                                             : "The object has no model loaded to fit.");

    if (object.HasComponent<Frost::BoxColliderComponent>()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("It also has a Box Collider, which is used instead: an object collides with one shape.");
        ImGui::PopStyleColor();
    }

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& /*context*/, Frost::GameObject& object,
                                   Frost::BoxColliderComponent& collider) {
    bool changed = false;

    changed |= ImGui::DragFloat3("Center", &collider.center.x, 1.0f * Frost::kCentimeter, 0.0f, 0.0f, "%.1f cm");
    ImGui::SetItemTooltip("The box's center, in the object's own space: it moves, turns and scales with the object.");
    changed |= ImGui::DragFloat3("Size", &collider.size.x, 1.0f * Frost::kCentimeter, 0.0f, 1.0f * Frost::kKilometer, "%.1f cm",
                                 ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Edge lengths, in the object's own space (the object's scale applies on top).");

    const std::optional<Frost::Bounds> bounds = ModelBounds(object);
    ImGui::BeginDisabled(!bounds.has_value());
    if (ImGui::SmallButton("Fit to Model")) {
        collider = Frost::BoxColliderComponent::FromBounds(*bounds);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(bounds.has_value() ? "Make the box enclose the object's model exactly."
                                             : "The object has no model loaded to fit.");

    const glm::vec3 worldSize = glm::abs(collider.size * object.GetTransform().scale);
    ImGui::TextDisabled("%.1f x %.1f x %.1f cm in the world", worldSize.x, worldSize.y, worldSize.z);

    const Frost::RigidBodyComponent* const body = object.TryGetComponent<Frost::RigidBodyComponent>();
    ImGui::TextDisabled("%s", body != nullptr && body->simulated ? "The shape of its simulated rigid body."
                                                                 : "Immovable: simulated bodies stop against it.");

    return changed;
}

bool InspectorPanel::DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::RigidBodyComponent& body) {
    bool changed = false;

    if (ImGui::Checkbox("Simulated", &body.simulated)) {
        body.linearVelocity = glm::vec3(0.0f);  // Starting or stopping, it does so from rest.
        changed = true;
    }
    ImGui::SetItemTooltip("Falls under gravity and collides when physics runs (in the game, or with Physics >\n"
                          "Simulate here). Unticked, the object stays where it is placed.");

    changed |= ImGui::DragFloat("Mass", &body.mass, 0.05f, 0.001f, 1.0e6f, "%.3f kg",
                                ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Only matters when two simulated bodies collide: the lighter one is pushed more.");

    const bool collides = object.HasComponent<Frost::BoxColliderComponent>() ||
                          object.HasComponent<Frost::CapsuleColliderComponent>();
    if (body.simulated && !collides) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("No collider: it will fall through everything. Add a Box or Capsule Collider to make it collide.");
        ImGui::PopStyleColor();
    }

    if (context.IsSimulating() && body.simulated) {
        const glm::vec3& velocity = body.linearVelocity;
        ImGui::TextDisabled("Velocity %.1f, %.1f, %.1f cm/s", velocity.x, velocity.y, velocity.z);
    }

    return changed;
}

template <typename Component>
bool InspectorPanel::DrawMaterials(EditorContext& context, Component& mesh, const Frost::MeshAsset* const asset) {
    if (mesh.GetMaterialSlotCount() == 0) {
        ImGui::TextDisabled("No model, so no materials.");
        return false;
    }

    bool changed = false;

    for (std::size_t i = 0; i < mesh.GetMaterialSlotCount(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        std::string partName;
        if (asset != nullptr && i < asset->mesh->GetSubMeshes().size()) { partName = asset->mesh->GetSubMeshes()[i].name; }
        const std::string header = "Part " + std::to_string(i) + (partName.empty() ? "" : ": " + partName);

        std::optional<Frost::MaterialOverride> materialOverride = mesh.GetMaterialOverride(i);

        if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            bool overridden = materialOverride.has_value();
            if (ImGui::Checkbox("Override material", &overridden)) {
                if (overridden) {
                    // Start from what the part looks like now: the model's own material (or the
                    // default one), with every texture slot inheriting.
                    const std::shared_ptr<Frost::Material>& imported =
                            asset != nullptr && i < asset->mesh->GetSubMeshes().size() ? asset->mesh->GetMaterial(i) : nullptr;
                    const Frost::Material& source =
                            imported != nullptr ? *imported : *context.GetAssets().GetMaterialSystem().GetDefaultMaterial();

                    materialOverride = Frost::MaterialOverride{.parameters = source.GetParameters(), .textures = {}};
                }
                else {
                    materialOverride.reset();
                }
                mesh.SetMaterialOverride(i, materialOverride);
                changed = true;
            }

            if (materialOverride.has_value()) {
                if (DrawMaterialOverride(context, *materialOverride)) {
                    mesh.SetMaterialOverride(i, materialOverride);
                    changed = true;
                }
            }
            else {
                ImGui::TextDisabled("Using the model's own material.");
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    return changed;
}

bool InspectorPanel::DrawMaterialOverride(EditorContext& context, Frost::MaterialOverride& materialOverride) {
    Frost::MaterialParameters& parameters = materialOverride.parameters;
    bool changed = false;

    // Base color is stored linear; the picker shows (and edits) its sRGB encoding.
    const glm::vec3 baseSrgb = EditorUI::LinearToSrgb(glm::vec3(parameters.baseColorFactor));
    float baseColor[4] = {baseSrgb.x, baseSrgb.y, baseSrgb.z, parameters.baseColorFactor.w};
    if (ImGui::ColorEdit4("Base Color", baseColor)) {
        parameters.baseColorFactor =
                glm::vec4(EditorUI::SrgbToLinear(glm::vec3(baseColor[0], baseColor[1], baseColor[2])), baseColor[3]);
        changed = true;
    }

    changed |= ImGui::SliderFloat("Metallic", &parameters.metallicFactor, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Roughness", &parameters.roughnessFactor, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Normal Scale", &parameters.normalScale, 0.0f, 2.0f);
    changed |= ImGui::SliderFloat("Occlusion", &parameters.occlusionStrength, 0.0f, 1.0f);

    // Emissive is light, not a surface color, so it is edited linearly and may exceed 1 (HDR).
    changed |= ImGui::ColorEdit3("Emissive", &parameters.emissiveFactor.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);

    ImGui::SeparatorText("Textures");
    if (ImGui::BeginTable("##TextureSlots", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Metal/Rough").x);
        ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch);

        for (std::size_t slot = 0; slot < Frost::kMaterialSlotCount; ++slot) {
            ImGui::PushID(static_cast<int>(slot));
            changed |= DrawTextureSlot(context, static_cast<Frost::MaterialSlot>(slot), materialOverride.textures[slot]);
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    return changed;
}

bool InspectorPanel::DrawTextureSlot(EditorContext& context, const Frost::MaterialSlot slot,
                                     Frost::TextureOverride& texture) {
    bool changed = false;

    std::string label;
    bool missing = false;
    std::optional<std::filesystem::path> current;
    switch (texture.source) {
        case Frost::TextureOverride::Source::Inherit: label = "(from model)"; break;
        case Frost::TextureOverride::Source::None: label = "(none)"; break;
        case Frost::TextureOverride::Source::Asset:
            current = CurrentPath(context, texture.texture);
            missing = !current.has_value();
            label = Frost::PathToUtf8(current.value_or(texture.texture.path).stem());
            if (missing) { label = "(missing) " + label; }
            break;
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(SlotLabel(slot));
    ImGui::TableNextColumn();

    if (missing) { ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor); }
    const bool openMenu = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f));
    if (missing) { ImGui::PopStyleColor(); }

    if (texture.source == Frost::TextureOverride::Source::Asset) {
        ImGui::SetItemTooltip("%s%s", Frost::PathToUtf8(current.value_or(texture.texture.path)).c_str(),
                              missing ? "\n(not found: it may have been deleted)" : "");
    }
    else {
        ImGui::SetItemTooltip("Drop an image from the Asset Library here, or click for options.");
    }

    if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetDrop(Frost::AssetType::Texture)) {
        if (!context.GetLibrary().IsInside(*dropped)) {
            context.ReportError("'" + Frost::PathToUtf8(dropped->filename()) + "' is not part of this project. A project "
                                "only uses its own assets: copy the file into the project's content folder first.");
        }
        else {
            texture = Frost::TextureOverride{.source = Frost::TextureOverride::Source::Asset,
                                             .texture = ReferenceTo(context, *dropped)};
            changed = true;
        }
    }

    // Click: pick from the library's images, or inherit/clear.
    if (openMenu) { ImGui::OpenPopup("TextureMenu"); }
    if (ImGui::BeginPopup("TextureMenu")) {
        if (ImGui::MenuItem("Use the model's texture", nullptr, texture.source == Frost::TextureOverride::Source::Inherit)) {
            texture = Frost::TextureOverride{};
            changed = true;
        }
        if (ImGui::MenuItem("None", nullptr, texture.source == Frost::TextureOverride::Source::None)) {
            texture = Frost::TextureOverride{.source = Frost::TextureOverride::Source::None, .texture = {}};
            changed = true;
        }

        ImGui::SeparatorText("Library");
        bool anyImages = false;
        for (const AssetLibrary::Entry& entry : context.GetLibrary().GetEntries()) {
            if (entry.type != Frost::AssetType::Texture) { continue; }
            anyImages = true;

            if (entry.duplicate) { continue; }
            const bool selected = texture.source == Frost::TextureOverride::Source::Asset && texture.texture.id == entry.id;
            const std::string name = Frost::PathToUtf8(entry.path.stem());
            if (ImGui::MenuItem(name.c_str(), nullptr, selected)) {
                texture = Frost::TextureOverride{.source = Frost::TextureOverride::Source::Asset,
                                                 .texture = {.id = entry.id, .path = entry.path}};
                changed = true;
            }
            ImGui::SetItemTooltip("%s", Frost::GenericPathToUtf8(entry.path.lexically_relative(context.GetLibrary().GetContentDirectory())).c_str());
        }
        if (!anyImages) { ImGui::TextDisabled("No images imported yet."); }

        ImGui::EndPopup();
    }

    return changed;
}
