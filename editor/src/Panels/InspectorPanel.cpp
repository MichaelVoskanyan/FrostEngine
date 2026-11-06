#include "InspectorPanel.hpp"

#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Renderer/MaterialSystem.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Units.hpp>

#include <imgui.h>

#include <glm/common.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>

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

    // A collider starts out wrapped around the object's model, if it has one.
    template <>
    [[nodiscard]]
    Frost::BoxColliderComponent MakeNewComponent<Frost::BoxColliderComponent>(const Frost::GameObject& object) {
        if (const Frost::MeshComponent* const mesh = object.TryGetComponent<Frost::MeshComponent>()) {
            if (const std::optional<Frost::Bounds> bounds = mesh->GetLocalBounds()) {
                return Frost::BoxColliderComponent::FromBounds(*bounds);
            }
        }
        return Frost::BoxColliderComponent{};
    }

    // A capsule likewise stands in the object's model, if it has one.
    template <>
    [[nodiscard]]
    Frost::CapsuleColliderComponent MakeNewComponent<Frost::CapsuleColliderComponent>(const Frost::GameObject& object) {
        if (const Frost::MeshComponent* const mesh = object.TryGetComponent<Frost::MeshComponent>()) {
            if (const std::optional<Frost::Bounds> bounds = mesh->GetLocalBounds()) {
                return Frost::CapsuleColliderComponent::FromBounds(*bounds);
            }
        }
        return Frost::CapsuleColliderComponent{};
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
        if constexpr (!std::is_same_v<T, Frost::TransformComponent>) {
            if (removeRequested) { removeComponent = [object] { object->RemoveComponent<T>(); }; }
        }
        ImGui::PopID();
    });

    if (removeComponent) {
        removeComponent();
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

    ImGui::SeparatorText("Materials");
    changed |= DrawMaterials(context, mesh);

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

    const Frost::MeshComponent* const mesh = object.TryGetComponent<Frost::MeshComponent>();
    const std::optional<Frost::Bounds> bounds = mesh != nullptr ? mesh->GetLocalBounds() : std::nullopt;
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

    const Frost::MeshComponent* const mesh = object.TryGetComponent<Frost::MeshComponent>();
    const std::optional<Frost::Bounds> bounds = mesh != nullptr ? mesh->GetLocalBounds() : std::nullopt;
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

bool InspectorPanel::DrawMaterials(EditorContext& context, Frost::MeshComponent& mesh) {
    if (mesh.GetMaterialSlotCount() == 0) {
        ImGui::TextDisabled("No model, so no materials.");
        return false;
    }

    bool changed = false;
    const std::shared_ptr<const Frost::MeshAsset>& asset = mesh.GetMesh();

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
