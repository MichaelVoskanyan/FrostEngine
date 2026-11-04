#include "LevelPanel.hpp"

#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Platform/Filesystem.hpp>

#include <imgui.h>

#include <glm/vec3.hpp>

#include <memory>
#include <optional>
#include <string>

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
        ImGui::Separator();
        for (const Frost::LightType type : {Frost::LightType::Directional, Frost::LightType::Point, Frost::LightType::Spot}) {
            if (ImGui::MenuItem(EditorUI::LightDisplayName(type))) { context.SpawnLight(type, context.GetSpawnPoint()); }
        }
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

    // Actions are applied after the loop: they create/destroy objects, which would invalidate it.
    std::optional<Frost::ObjectId> toDelete;
    bool duplicate = false;

    if (ImGui::BeginChild("##Objects")) {
        const Frost::GameObject* const selected = context.GetSelectedObject();

        for (const std::unique_ptr<Frost::GameObject>& object : level.GetObjects()) {
            ImGui::PushID(static_cast<int>(object->GetId()));

            // Objects whose model failed to load are flagged, so broken references are easy to spot.
            const bool missingMesh = object->GetMesh() == nullptr && object->GetMeshReference().IsSet();
            const std::string label = object->GetName().empty() ? "(unnamed)" : object->GetName();

            if (missingMesh) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f)); }
            if (ImGui::Selectable(label.c_str(), object.get() == selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                context.Select(object->GetId());
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { context.RequestFocus(object->GetId()); }
            }
            if (missingMesh) {
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Model not found: %s", Frost::PathToUtf8(object->GetMeshReference().path).c_str());
            }


            if (ImGui::BeginPopupContextItem()) {
                context.Select(object->GetId());
                if (ImGui::MenuItem("Focus", "F")) { context.RequestFocus(object->GetId()); }
                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) { duplicate = true; }
                if (ImGui::MenuItem("Delete", "Del")) { toDelete = object->GetId(); }
                ImGui::EndPopup();
            }

            // Lights have no geometry to recognize them by; tag them (dimmed when switched off).
            // After the context menu, which attaches to the last item (the Selectable).
            if (const std::optional<Frost::LightComponent>& light = object->GetLight(); light.has_value()) {
                const char* const tag = light->enabled ? "light" : "light (off)";
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(tag).x);
                ImGui::TextDisabled("%s", tag);
            }

            ImGui::PopID();
        }

        if (level.GetObjects().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("No objects. Drag a model from the Asset Library into the viewport or this list.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    // The whole list area accepts models.
    if (const std::optional<std::filesystem::path> dropped = EditorUI::AcceptAssetDrop(Frost::AssetType::Mesh)) {
        context.SpawnMesh(*dropped, context.GetSpawnPoint());
    }

    if (duplicate) { context.DuplicateSelected(); }
    if (toDelete.has_value()) { context.DeleteObject(*toDelete); }

    ImGui::End();
}
