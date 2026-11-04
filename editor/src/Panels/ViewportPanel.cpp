#include "ViewportPanel.hpp"

#include "../EditorCamera.hpp"
#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Engine.hpp>
#include <Frost/Renderer/Renderer.hpp>

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {

    // Drops that hit nothing land on the ground plane, unless that is further than this (looking
    // almost level with the horizon), in which case they land in front of the camera instead.
    constexpr float kMaxGroundDropDistance = 200.0f;
    constexpr float kFallbackDropDistance = 5.0f;

    constexpr ImU32 kSelectionColor = IM_COL32(255, 160, 40, 255);

    // Light icons: radius on screen (also the click target), and how long the direction arrow of
    // directional/spot lights is in world units.
    constexpr float kLightIconRadius = 9.0f;
    constexpr float kLightArrowLength = 1.0f;

    // Segments used to draw circles (range spheres, cone rims).
    constexpr int kCircleSegments = 48;

    [[nodiscard]]
    char LightTypeLetter(const Frost::LightType type) {
        switch (type) {
            case Frost::LightType::Directional: return 'D';
            case Frost::LightType::Point: return 'P';
            case Frost::LightType::Spot: return 'S';
        }
        return '?';
    }

    // Two unit vectors perpendicular to `direction` and to each other.
    void PerpendicularBasis(const glm::vec3& direction, glm::vec3& u, glm::vec3& v) {
        const glm::vec3 helper = std::abs(direction.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        u = glm::normalize(glm::cross(direction, helper));
        v = glm::cross(u, direction);
    }

    // The light's tint (color and temperature, without intensity) as a display color.
    [[nodiscard]]
    ImU32 LightDisplayColor(const Frost::LightComponent& light, const int alpha = 255) {
        const glm::vec3 tint = light.useTemperature ? light.color * Frost::ColorTemperatureToLinearRgb(light.temperature)
                                                    : light.color;
        const glm::vec3 srgb = glm::clamp(EditorUI::LinearToSrgb(glm::clamp(tint, 0.0f, 1.0f)), 0.0f, 1.0f);
        return IM_COL32(static_cast<int>(srgb.x * 255.0f), static_cast<int>(srgb.y * 255.0f),
                        static_cast<int>(srgb.z * 255.0f), alpha);
    }

}  // namespace

void ViewportPanel::Draw(EditorContext& context, const EditorCamera& camera) {
    Frost::Renderer& renderer = context.GetEngine().GetRenderer();

    // The scene is the window's content; no padding, no background (the renderer draws behind it),
    // and no tab bar unless another window is docked alongside.
    ImGuiWindowClass windowClass;
    windowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
    ImGui::SetNextWindowClass(&windowClass);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin(kWindowName, nullptr,
                                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                                              ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();

    m_Hovered = false;
    m_Origin = ImGui::GetCursorScreenPos();
    m_Size = ImGui::GetContentRegionAvail();

    if (!visible || m_Size.x < 1.0f || m_Size.y < 1.0f) {
        // Hidden (another tab in front) or collapsed to nothing: draw no scene at all.
        renderer.SetSceneViewport(Frost::Renderer::SceneViewport{});
        ImGui::End();
        return;
    }

    // ImGui works in window coordinates; the renderer in framebuffer pixels (they differ on
    // high-DPI displays).
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    renderer.SetSceneViewport(Frost::Renderer::SceneViewport{
            .x = static_cast<std::int32_t>(std::lround(m_Origin.x * scale.x)),
            .y = static_cast<std::int32_t>(std::lround(m_Origin.y * scale.y)),
            .width = static_cast<std::uint32_t>(std::lround(m_Size.x * scale.x)),
            .height = static_cast<std::uint32_t>(std::lround(m_Size.y * scale.y)),
    });

    const Projection projection = MakeProjection(camera);

    // One invisible item covering the scene area receives clicks and drops.
    ImGui::InvisibleButton("##Scene", m_Size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
    m_Hovered = ImGui::IsItemHovered();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        // Light icons first: they are drawn on top of everything, so that is what the user sees
        // under the cursor.
        if (Frost::GameObject* const light = PickLightIcon(context, projection, ImGui::GetMousePos())) {
            context.Select(light->GetId());
        }
        else {
            const std::optional<Frost::RaycastHit> hit =
                    context.GetLevel().Raycast(ScreenPointToRay(camera, ImGui::GetMousePos()));
            context.Select(hit.has_value() ? hit->object->GetId() : 0);
        }
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (const Frost::GameObject* const selected = context.GetSelectedObject(); selected != nullptr) {
            context.RequestFocus(selected->GetId());
        }
    }

    // Models from the Asset Library, and new lights from its Lights tab.
    if (ImGui::BeginDragDropTarget()) {
        const Frost::Ray ray = ScreenPointToRay(camera, ImGui::GetMousePos());

        if (const std::optional<std::filesystem::path> mesh = EditorUI::AcceptAssetPayload(Frost::AssetType::Mesh)) {
            context.SpawnMesh(*mesh, FindDropPoint(context, ray));
        }
        if (const std::optional<Frost::LightType> light = EditorUI::AcceptLightPayload()) {
            context.SpawnLight(*light, FindDropPoint(context, ray));
        }

        ImGui::EndDragDropTarget();
    }

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(m_Origin, ImVec2(m_Origin.x + m_Size.x, m_Origin.y + m_Size.y), true);
    DrawSelectionBounds(context, projection);
    DrawLights(context, projection);
    drawList->PopClipRect();

    DrawCameraInfo(camera);

    ImGui::End();
}

bool ViewportPanel::IsHovered() const noexcept { return m_Hovered; }

Frost::Ray ViewportPanel::ScreenPointToRay(const EditorCamera& camera, const ImVec2& screenPoint) const {
    const Frost::Camera& view = camera.GetCamera();

    // Normalized device coordinates of the point, +Y up (the renderer's clip space convention).
    const float x = m_Size.x > 0.0f ? (screenPoint.x - m_Origin.x) / m_Size.x * 2.0f - 1.0f : 0.0f;
    const float y = m_Size.y > 0.0f ? 1.0f - (screenPoint.y - m_Origin.y) / m_Size.y * 2.0f : 0.0f;

    const float aspect = m_Size.y > 0.0f ? m_Size.x / m_Size.y : 1.0f;
    const float tanHalfFov = std::tan(view.GetVerticalFov() * 0.5f);

    const glm::vec3 direction = view.GetForward() + view.GetRight() * (x * tanHalfFov * aspect) +
                                view.GetUp() * (y * tanHalfFov);

    return Frost::Ray{.origin = view.GetPosition(), .direction = glm::normalize(direction)};
}

ViewportPanel::Projection ViewportPanel::MakeProjection(const EditorCamera& camera) const {
    const Frost::Camera& view = camera.GetCamera();
    return Projection{.view = view.GetViewMatrix(),
                      .projection = view.GetProjectionMatrix(m_Size.y > 0.0f ? m_Size.x / m_Size.y : 1.0f),
                      .nearClip = view.GetNearClip()};
}

std::optional<ImVec2> ViewportPanel::WorldToScreen(const Projection& projection, const glm::vec3& world) const {
    const glm::vec4 clip = projection.projection * projection.view * glm::vec4(world, 1.0f);
    if (clip.w < projection.nearClip) { return std::nullopt; }  // clip.w is the distance in front of the camera.

    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    return ImVec2(m_Origin.x + (ndcX * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - ndcY * 0.5f) * m_Size.y);
}

void ViewportPanel::DrawWorldLine(const Projection& projection, const glm::vec3& a, const glm::vec3& b,
                                  const ImU32 color, const float thickness) const {
    glm::vec3 viewA = glm::vec3(projection.view * glm::vec4(a, 1.0f));
    glm::vec3 viewB = glm::vec3(projection.view * glm::vec4(b, 1.0f));

    // Clip against the near plane (view space looks down -Z): a line wholly behind it is not
    // drawn, one crossing it is cut at it, so nothing projects through the camera.
    const float limit = -projection.nearClip;
    if (viewA.z > limit && viewB.z > limit) { return; }
    if (viewA.z > limit) { viewA = viewB + (viewA - viewB) * ((limit - viewB.z) / (viewA.z - viewB.z)); }
    if (viewB.z > limit) { viewB = viewA + (viewB - viewA) * ((limit - viewA.z) / (viewB.z - viewA.z)); }

    const auto toScreen = [&](const glm::vec3& viewPoint) {
        const glm::vec4 clip = projection.projection * glm::vec4(viewPoint, 1.0f);
        return ImVec2(m_Origin.x + (clip.x / clip.w * 0.5f + 0.5f) * m_Size.x,
                      m_Origin.y + (0.5f - clip.y / clip.w * 0.5f) * m_Size.y);
    };

    ImGui::GetWindowDrawList()->AddLine(toScreen(viewA), toScreen(viewB), color, thickness);
}

void ViewportPanel::DrawWorldCircle(const Projection& projection, const glm::vec3& center, const glm::vec3& axisU,
                                    const glm::vec3& axisV, const float radius, const ImU32 color) const {
    glm::vec3 previous = center + axisU * radius;
    for (int i = 1; i <= kCircleSegments; ++i) {
        const float angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(kCircleSegments);
        const glm::vec3 point = center + (axisU * std::cos(angle) + axisV * std::sin(angle)) * radius;
        DrawWorldLine(projection, previous, point, color, 1.0f);
        previous = point;
    }
}

glm::vec3 ViewportPanel::FindDropPoint(EditorContext& context, const Frost::Ray& ray) const {
    if (const std::optional<Frost::RaycastHit> hit = context.GetLevel().Raycast(ray); hit.has_value()) {
        return hit->point;
    }

    if (ray.direction.y < -1.0e-4f) {
        const float distance = -ray.origin.y / ray.direction.y;
        if (distance > 0.0f && distance < kMaxGroundDropDistance) { return ray.origin + ray.direction * distance; }
    }

    return ray.origin + ray.direction * kFallbackDropDistance;
}

Frost::GameObject* ViewportPanel::PickLightIcon(EditorContext& context, const Projection& projection,
                                                const ImVec2& screenPoint) const {
    Frost::GameObject* nearest = nullptr;
    float nearestDepth = std::numeric_limits<float>::max();

    for (const std::unique_ptr<Frost::GameObject>& object : context.GetLevel().GetObjects()) {
        if (!object->GetLight().has_value()) { continue; }

        const glm::vec3& position = object->GetTransform().position;
        const std::optional<ImVec2> icon = WorldToScreen(projection, position);
        if (!icon.has_value()) { continue; }

        const float dx = icon->x - screenPoint.x;
        const float dy = icon->y - screenPoint.y;
        if (dx * dx + dy * dy > kLightIconRadius * kLightIconRadius) { continue; }

        const float depth = -(projection.view * glm::vec4(position, 1.0f)).z;
        if (depth < nearestDepth) {
            nearestDepth = depth;
            nearest = object.get();
        }
    }

    return nearest;
}

void ViewportPanel::DrawSelectionBounds(EditorContext& context, const Projection& projection) const {
    const Frost::GameObject* const selected = context.GetSelectedObject();
    if (selected == nullptr) { return; }

    const std::optional<Frost::Bounds> bounds = selected->GetWorldBounds();
    if (!bounds.has_value()) { return; }

    const auto corner = [&](const int i) {
        return glm::vec3((i & 1) != 0 ? bounds->max.x : bounds->min.x, (i & 2) != 0 ? bounds->max.y : bounds->min.y,
                         (i & 4) != 0 ? bounds->max.z : bounds->min.z);
    };

    // The 12 edges of the box, as corner index pairs differing in exactly one axis bit.
    constexpr std::array<std::pair<int, int>, 12> kEdges = {{{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                                            {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                                            {0, 4}, {1, 5}, {2, 6}, {3, 7}}};

    for (const auto& [first, second] : kEdges) {
        DrawWorldLine(projection, corner(first), corner(second), kSelectionColor);
    }
}

void ViewportPanel::DrawLights(EditorContext& context, const Projection& projection) const {
    const Frost::GameObject* const selected = context.GetSelectedObject();
    ImDrawList* const drawList = ImGui::GetWindowDrawList();

    for (const std::unique_ptr<Frost::GameObject>& object : context.GetLevel().GetObjects()) {
        const std::optional<Frost::LightComponent>& light = object->GetLight();
        if (!light.has_value()) { continue; }

        const bool isSelected = object.get() == selected;
        const Frost::Transform& transform = object->GetTransform();

        if (isSelected) { DrawLightReach(projection, *object); }

        // Which way directional and spot lights shine.
        if (light->type != Frost::LightType::Point) {
            DrawWorldLine(projection, transform.position, transform.position + transform.GetForward() * kLightArrowLength,
                          isSelected ? kSelectionColor : LightDisplayColor(*light, 200), 2.0f);
        }

        const std::optional<ImVec2> icon = WorldToScreen(projection, transform.position);
        if (!icon.has_value()) { continue; }

        // A disc in the light's color with its type letter; disabled lights are hollow and grey.
        const ImU32 fill = light->enabled ? LightDisplayColor(*light) : IM_COL32(60, 60, 60, 200);
        const ImU32 border = isSelected ? kSelectionColor : IM_COL32(20, 20, 20, 255);
        drawList->AddCircleFilled(*icon, kLightIconRadius, fill);
        drawList->AddCircle(*icon, kLightIconRadius, border, 0, isSelected ? 2.5f : 1.5f);

        const char letter[2] = {LightTypeLetter(light->type), '\0'};
        const ImVec2 textSize = ImGui::CalcTextSize(letter);
        drawList->AddText(ImVec2(icon->x - textSize.x * 0.5f, icon->y - textSize.y * 0.5f),
                          light->enabled ? IM_COL32(0, 0, 0, 255) : IM_COL32(170, 170, 170, 255), letter);
    }
}

void ViewportPanel::DrawLightReach(const Projection& projection, const Frost::GameObject& object) const {
    const Frost::LightComponent& light = *object.GetLight();
    const Frost::Transform& transform = object.GetTransform();
    const glm::vec3& position = transform.position;
    const glm::vec3 forward = transform.GetForward();

    glm::vec3 u;
    glm::vec3 v;
    PerpendicularBasis(forward, u, v);

    switch (light.type) {
        case Frost::LightType::Point: {
            // The sphere the light reaches, as its three axis-aligned great circles.
            const float radius = std::max(light.range, 0.0f);
            DrawWorldCircle(projection, position, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), radius, kSelectionColor);
            DrawWorldCircle(projection, position, glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), radius, kSelectionColor);
            DrawWorldCircle(projection, position, glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), radius, kSelectionColor);
            break;
        }

        case Frost::LightType::Spot: {
            // The outer cone out to the light's range (slant length = range), its rim, and the
            // inner cone's rim, fainter.
            const float outer = glm::radians(std::clamp(light.outerConeAngle, 0.1f, 89.9f));
            const float inner = glm::radians(std::clamp(light.innerConeAngle, 0.0f, glm::degrees(outer)));
            const float range = std::max(light.range, 0.0f);

            const glm::vec3 outerCenter = position + forward * (range * std::cos(outer));
            const float outerRadius = range * std::sin(outer);
            DrawWorldCircle(projection, outerCenter, u, v, outerRadius, kSelectionColor);
            for (const glm::vec3& side : {u, -u, v, -v}) {
                DrawWorldLine(projection, position, outerCenter + side * outerRadius, kSelectionColor, 1.0f);
            }

            DrawWorldCircle(projection, position + forward * (range * std::cos(inner)), u, v, range * std::sin(inner),
                            IM_COL32(255, 160, 40, 110));
            break;
        }

        case Frost::LightType::Directional: {
            // A bundle of parallel rays: the light has no position, only a direction.
            constexpr float kRayLength = 3.0f;
            constexpr float kSpread = 0.4f;
            for (const glm::vec3& offset : {glm::vec3(0.0f), u * kSpread, -u * kSpread, v * kSpread, -v * kSpread}) {
                DrawWorldLine(projection, position + offset, position + offset + forward * kRayLength, kSelectionColor,
                              1.0f);
            }
            break;
        }
    }
}

void ViewportPanel::DrawCameraInfo(const EditorCamera& camera) const {
    const glm::vec3& position = camera.GetPosition();

    char text[160];
    std::snprintf(text, sizeof(text), "Speed %.2f u/s   Pos %.1f, %.1f, %.1f%s", camera.GetMoveSpeed(), position.x,
                  position.y, position.z, camera.IsControlling() ? "" : "   (hold RMB to fly)");

    const ImVec2 at(m_Origin.x + 8.0f, m_Origin.y + 6.0f);
    ImGui::GetWindowDrawList()->AddText(at, IM_COL32(230, 230, 230, 200), text);
}
