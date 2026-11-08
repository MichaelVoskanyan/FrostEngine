// DebugDraw::Render: drawn through Dear ImGui, which the engine links privately (no engine header
// includes it). A native line renderer (depth tested) can replace this without changing callers.

#include <Frost/Debug/DebugDraw.hpp>

#include <Frost/ImGui/ImGuiLayer.hpp>
#include <Frost/Renderer/Camera.hpp>

#include <imgui.h>

#include <cmath>

namespace {

    [[nodiscard]]
    float SrgbToLinear(const float value) noexcept {
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    }

    // The color as ImGui draws it: encoded as the target needs (see ImGuiLayer::IsOutputLinear).
    [[nodiscard]]
    ImU32 ToImGuiColor(const Frost::DebugColor& color) {
        if (Frost::ImGuiLayer::IsOutputLinear()) {
            return ImGui::ColorConvertFloat4ToU32(
                    ImVec4(SrgbToLinear(color.r), SrgbToLinear(color.g), SrgbToLinear(color.b), color.a));
        }
        return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a));
    }

}  // namespace

namespace Frost {

    void DebugDraw::Render(const Camera& camera) const {
        if (IsEmpty() || ImGui::GetCurrentContext() == nullptr) { return; }

        const ImGuiViewport* const viewport = ImGui::GetMainViewport();
        const glm::vec2 screenSize(viewport->Size.x, viewport->Size.y);
        if (!(screenSize.x > 0.0f && screenSize.y > 0.0f)) { return; }  // Minimized.

        const glm::vec2 origin(viewport->Pos.x, viewport->Pos.y);
        const glm::mat4 view = camera.GetViewMatrix();
        const glm::mat4 projection = camera.GetProjectionMatrix(screenSize.x / screenSize.y);
        const float nearClip = camera.GetNearClip();

        // Behind any ImGui windows, over the scene.
        ImDrawList* const drawList = ImGui::GetBackgroundDrawList();
        for (const Line& line : m_Lines) {
            if (const std::optional<ScreenLine> onScreen =
                        ProjectLineToScreen(view, projection, nearClip, screenSize, line.from, line.to)) {
                drawList->AddLine(ImVec2(origin.x + onScreen->from.x, origin.y + onScreen->from.y),
                                  ImVec2(origin.x + onScreen->to.x, origin.y + onScreen->to.y), ToImGuiColor(line.color),
                                  line.thickness);
            }
        }
        for (const Label& label : m_Labels) {
            if (const std::optional<glm::vec2> at = ProjectToScreen(view, projection, nearClip, screenSize, label.position)) {
                // A dark shadow keeps the text readable over bright surfaces.
                const ImVec2 position(origin.x + at->x + 6.0f, origin.y + at->y - 6.0f);
                drawList->AddText(ImVec2(position.x + 1.0f, position.y + 1.0f), IM_COL32(0, 0, 0, 200), label.text.c_str());
                drawList->AddText(position, ToImGuiColor(label.color), label.text.c_str());
            }
        }
    }

}  // namespace Frost
