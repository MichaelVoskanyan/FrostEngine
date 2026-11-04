#include "InspectorPanel.hpp"

#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Renderer/MaterialSystem.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <imgui.h>

#include <glm/common.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <system_error>

namespace {

    constexpr ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);

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

}  // namespace

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

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) { changed |= DrawTransform(*object); }

    // A light without a model is what lights usually are; keep the model sections out of the way.
    const bool isBareLight = object->GetLight().has_value() && !object->GetMeshReference().IsSet();
    const ImGuiTreeNodeFlags modelFlags = isBareLight ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen;

    if (object->GetLight().has_value() && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= DrawLight(*object);
    }
    if (ImGui::CollapsingHeader("Model", modelFlags)) { changed |= DrawMesh(context, *object); }
    if (ImGui::CollapsingHeader("Materials", modelFlags)) { changed |= DrawMaterials(context, *object); }

    if (!object->GetLight().has_value()) {
        ImGui::Spacing();
        if (ImGui::Button("Add Light")) { ImGui::OpenPopup("AddLight"); }
        if (ImGui::BeginPopup("AddLight")) {
            for (const Frost::LightType type : {Frost::LightType::Directional, Frost::LightType::Point, Frost::LightType::Spot}) {
                if (ImGui::MenuItem(EditorUI::LightDisplayName(type))) {
                    object->GetLight() = Frost::LightComponent::CreateDefault(type);
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
    }

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

bool InspectorPanel::DrawTransform(Frost::GameObject& object) {
    Frost::Transform& transform = object.GetTransform();
    bool changed = false;

    changed |= ImGui::DragFloat3("Position", &transform.position.x, 0.05f, 0.0f, 0.0f, "%.3f");
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
        transform = Frost::Transform{.position = transform.position};
        changed = true;
    }
    ImGui::SetItemTooltip("Reset rotation and scale (keeps the position).");

    return changed;
}

bool InspectorPanel::DrawLight(Frost::GameObject& object) {
    Frost::LightComponent& light = *object.GetLight();
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
                                  : "Brightness at 1 unit away; falls off with the square of the distance.");

    if (light.type != Frost::LightType::Directional) {
        const char* const rangeLabel = light.type == Frost::LightType::Point ? "Radius" : "Distance";
        changed |= ImGui::DragFloat(rangeLabel, &light.range, std::max(0.01f, light.range * 0.01f), 0.01f, 1.0e5f,
                                    "%.2f", ImGuiSliderFlags_AlwaysClamp);
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

    ImGui::Spacing();
    if (ImGui::SmallButton("Remove Light")) {
        object.GetLight().reset();  // `light` dangles from here on.
        return true;
    }

    return changed;
}

bool InspectorPanel::DrawMesh(EditorContext& context, Frost::GameObject& object) {
    bool changed = false;

    const Frost::AssetReference& reference = object.GetMeshReference();
    const std::optional<std::filesystem::path> current = reference.IsSet() ? CurrentPath(context, reference) : std::nullopt;
    const bool missing = reference.IsSet() && (object.GetMesh() == nullptr || !current.has_value());

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
            object.SetMesh(context.GetAssets().LoadMesh(*dropped));
            changed = true;
        }
        catch (const Frost::ImportError& e) {
            context.ReportError(std::string("Could not load the model: ") + e.what());
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!reference.IsSet());
    if (ImGui::Button("Clear", ImVec2(-FLT_MIN, 0.0f))) {
        object.SetMesh(nullptr);
        changed = true;
    }
    ImGui::EndDisabled();

    if (object.GetMesh() != nullptr) {
        const Frost::Mesh& mesh = *object.GetMesh()->mesh;
        ImGui::TextDisabled("%zu part(s), %u vertices, %u triangles", mesh.GetSubMeshes().size(), mesh.GetVertexCount(),
                            mesh.GetIndexCount() / 3);
    }

    return changed;
}

bool InspectorPanel::DrawMaterials(EditorContext& context, Frost::GameObject& object) {
    if (object.GetMaterialSlotCount() == 0) {
        ImGui::TextDisabled("No model, so no materials.");
        return false;
    }

    bool changed = false;
    const std::shared_ptr<const Frost::MeshAsset>& asset = object.GetMesh();

    for (std::size_t i = 0; i < object.GetMaterialSlotCount(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        std::string partName;
        if (asset != nullptr && i < asset->mesh->GetSubMeshes().size()) { partName = asset->mesh->GetSubMeshes()[i].name; }
        const std::string header = "Part " + std::to_string(i) + (partName.empty() ? "" : ": " + partName);

        std::optional<Frost::MaterialOverride> materialOverride = object.GetMaterialOverride(i);

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
                object.SetMaterialOverride(i, materialOverride);
                changed = true;
            }

            if (materialOverride.has_value()) {
                if (DrawMaterialOverride(context, *materialOverride)) {
                    object.SetMaterialOverride(i, materialOverride);
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
