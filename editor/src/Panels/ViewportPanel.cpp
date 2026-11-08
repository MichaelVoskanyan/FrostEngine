#include "ViewportPanel.hpp"

#include "../EditorCamera.hpp"
#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Engine.hpp>
#include <Frost/Physics/Collision.hpp>
#include <Frost/Renderer/Renderer.hpp>
#include <Frost/Scene/Hierarchy.hpp>
#include <Frost/Units.hpp>

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
    constexpr float kMaxGroundDropDistance = 200.0f * Frost::kMeter;
    constexpr float kFallbackDropDistance = 5.0f * Frost::kMeter;

    constexpr ImU32 kSelectionColor = IM_COL32(255, 160, 40, 255);

    // The selected skeletal mesh's skeleton.
    constexpr ImU32 kSkeletonColor = IM_COL32(240, 230, 120, 220);
    constexpr ImU32 kSkeletonJointColor = IM_COL32(255, 250, 190, 255);

    // Box colliders: immovable ones, and those of simulated rigid bodies (brighter when selected).
    constexpr ImU32 kStaticColliderColor = IM_COL32(90, 220, 120, 140);
    constexpr ImU32 kStaticColliderSelectedColor = IM_COL32(120, 255, 150, 255);
    constexpr ImU32 kSimulatedColliderColor = IM_COL32(90, 170, 255, 150);
    constexpr ImU32 kSimulatedColliderSelectedColor = IM_COL32(130, 200, 255, 255);

    constexpr ImU32 kSimulatingTextColor = IM_COL32(255, 120, 90, 255);
    constexpr ImU32 kPlayingTextColor = IM_COL32(120, 220, 255, 255);

    // Camera objects: their icon, and how far out the shape of their view is drawn.
    constexpr ImU32 kCameraColor = IM_COL32(220, 220, 220, 200);
    constexpr float kCameraFrustumLength = 60.0f * Frost::kCentimeter;
    constexpr float kCameraFrustumAspect = 16.0f / 9.0f;

    // The 12 edges of a box, as pairs of corner indices (see OrientedBox::GetCorners) that differ in
    // exactly one axis bit.
    constexpr std::array<std::pair<int, int>, 12> kBoxEdges = {{{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                                               {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                                               {0, 4}, {1, 5}, {2, 6}, {3, 7}}};

    // Light icons: radius on screen, in pixels (also the click target), and how long the direction
    // arrow of directional/spot lights is in the world.
    constexpr float kLightIconRadius = 9.0f;
    constexpr float kLightArrowLength = 1.0f * Frost::kMeter;

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
        if (Frost::GameObject* const icon = PickIcon(context, projection, ImGui::GetMousePos())) {
            context.Select(icon->GetId());
        }
        else {
            const std::optional<Frost::MeshRaycastHit> hit =
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

        for (const Frost::AssetType type : {Frost::AssetType::Mesh, Frost::AssetType::SkeletalMesh}) {
            if (const std::optional<std::filesystem::path> model = EditorUI::AcceptAssetPayload(type)) {
                context.SpawnModel(*model, FindDropPoint(context, ray));
            }
        }
        if (const std::optional<Frost::LightType> light = EditorUI::AcceptLightPayload()) {
            context.SpawnLight(*light, FindDropPoint(context, ray));
        }

        ImGui::EndDragDropTarget();
    }

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(m_Origin, ImVec2(m_Origin.x + m_Size.x, m_Origin.y + m_Size.y), true);
    if (m_ShowColliders) { DrawColliders(context, projection); }
    DrawSelectionBounds(context, projection);
    DrawSelectedSkeleton(context, projection);
    DrawLights(context, projection);
    DrawCameras(context, projection);
    drawList->PopClipRect();

    DrawCameraInfo(context, camera);

    ImGui::End();
}

bool ViewportPanel::IsHovered() const noexcept { return m_Hovered; }

bool ViewportPanel::GetShowColliders() const noexcept { return m_ShowColliders; }

void ViewportPanel::SetShowColliders(const bool show) noexcept { m_ShowColliders = show; }

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
    if (const std::optional<Frost::MeshRaycastHit> hit = context.GetLevel().Raycast(ray); hit.has_value()) {
        return hit->point;
    }

    if (ray.direction.y < -1.0e-4f) {
        const float distance = -ray.origin.y / ray.direction.y;
        if (distance > 0.0f && distance < kMaxGroundDropDistance) { return ray.origin + ray.direction * distance; }
    }

    return ray.origin + ray.direction * kFallbackDropDistance;
}

Frost::GameObject* ViewportPanel::PickIcon(EditorContext& context, const Projection& projection,
                                           const ImVec2& screenPoint) const {
    Frost::GameObject* nearest = nullptr;
    float nearestDepth = std::numeric_limits<float>::max();

    for (const std::unique_ptr<Frost::GameObject>& object : context.GetLevel().GetObjects()) {
        // A camera's icon is where the camera is (mounted on its object, e.g. at a character's head).
        const Frost::WorldPlacement placement = Frost::ComputeWorldPlacement(context.GetLevel().GetRegistry(), object->GetId());
        glm::vec3 position = placement.position;
        if (const Frost::CameraComponent* const camera = object->TryGetComponent<Frost::CameraComponent>()) {
            if (!object->HasComponent<Frost::LightComponent>()) {
                position = camera->ToCamera(placement.position, placement.rotation).GetPosition();
            }
        }
        else if (!object->HasComponent<Frost::LightComponent>()) {
            continue;
        }
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

    for (const auto& [first, second] : kBoxEdges) {
        DrawWorldLine(projection, corner(first), corner(second), kSelectionColor);
    }
}

void ViewportPanel::DrawSelectedSkeleton(EditorContext& context, const Projection& projection) const {
    const Frost::GameObject* const selected = context.GetSelectedObject();
    if (selected == nullptr) { return; }

    const Frost::SkeletalMeshComponent* const mesh = selected->TryGetComponent<Frost::SkeletalMeshComponent>();
    if (mesh == nullptr || mesh->GetSkeletalMesh() == nullptr || mesh->GetSkeletalMesh()->skeleton == nullptr) { return; }

    const Frost::SkeletonData& skeleton = mesh->GetSkeletalMesh()->skeleton->skeleton;
    const std::vector<glm::mat4>& bones = mesh->GetBoneTransforms();
    if (bones.size() != skeleton.bones.size()) { return; }

    const glm::mat4 model = selected->GetWorldMatrix();
    const auto jointOf = [&](const std::size_t bone) { return glm::vec3(model * bones[bone][3]); };

    // Bones from their parent's joint to their own, then the joints on top.
    for (std::size_t bone = 0; bone < skeleton.bones.size(); ++bone) {
        const std::int32_t parent = skeleton.bones[bone].parent;
        if (parent >= 0) { DrawWorldLine(projection, jointOf(static_cast<std::size_t>(parent)), jointOf(bone), kSkeletonColor, 2.0f); }
    }
    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    for (std::size_t bone = 0; bone < skeleton.bones.size(); ++bone) {
        if (const std::optional<ImVec2> joint = WorldToScreen(projection, jointOf(bone))) {
            drawList->AddCircleFilled(*joint, 3.0f, kSkeletonJointColor);
        }
    }
}

void ViewportPanel::DrawColliders(EditorContext& context, const Projection& projection) const {
    const Frost::GameObject* const selected = context.GetSelectedObject();

    for (const std::unique_ptr<Frost::GameObject>& object : context.GetLevel().GetObjects()) {
        const Frost::BoxColliderComponent* const box = object->TryGetComponent<Frost::BoxColliderComponent>();
        const Frost::CapsuleColliderComponent* const capsule = object->TryGetComponent<Frost::CapsuleColliderComponent>();
        if (box == nullptr && capsule == nullptr) { continue; }

        const Frost::RigidBodyComponent* const body = object->TryGetComponent<Frost::RigidBodyComponent>();
        const bool simulated = body != nullptr && body->simulated;
        const bool isSelected = object.get() == selected;

        ImU32 color = simulated ? kSimulatedColliderColor : kStaticColliderColor;
        if (isSelected) { color = simulated ? kSimulatedColliderSelectedColor : kStaticColliderSelectedColor; }
        const float thickness = isSelected ? 2.0f : 1.0f;

        // (An attached object's colliders are shown where the object is, though they do nothing
        // while it is attached.)
        Frost::TransformComponent transform = object->GetTransform();
        if (Frost::IsAttached(context.GetLevel().GetRegistry(), object->GetId())) { transform.SetFromMatrix(object->GetWorldMatrix()); }

        if (box != nullptr) {
            const std::array<glm::vec3, 8> corners = Frost::MakeWorldBox(transform, *box).GetCorners();
            for (const auto& [first, second] : kBoxEdges) {
                DrawWorldLine(projection, corners[static_cast<std::size_t>(first)],
                              corners[static_cast<std::size_t>(second)], color, thickness);
            }
        }
        if (capsule != nullptr) {
            DrawCapsule(projection, Frost::MakeWorldCapsule(transform, *capsule), color, thickness);
        }
    }
}

void ViewportPanel::DrawCapsule(const Projection& projection, const Frost::Capsule& capsule, const ImU32 color,
                                const float thickness) const {
    // Its axis, and two directions across it.
    const glm::vec3 span = capsule.end - capsule.start;
    const glm::vec3 axis = glm::dot(span, span) > 1e-10f ? glm::normalize(span) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 u;
    glm::vec3 v;
    PerpendicularBasis(axis, u, v);
    const float r = capsule.radius;

    // A point on a circle of radius r around `center` in the plane of `a` and `b`, at `angle`.
    const auto onCircle = [r](const glm::vec3& center, const glm::vec3& a, const glm::vec3& b, const float angle) {
        return center + (a * std::cos(angle) + b * std::sin(angle)) * r;
    };

    // The rims of the cylinder, and four lines along its side.
    for (const glm::vec3& center : {capsule.start, capsule.end}) {
        glm::vec3 previous = onCircle(center, u, v, 0.0f);
        for (int i = 1; i <= kCircleSegments; ++i) {
            const float angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(kCircleSegments);
            const glm::vec3 point = onCircle(center, u, v, angle);
            DrawWorldLine(projection, previous, point, color, thickness);
            previous = point;
        }
    }
    for (const glm::vec3& side : {u, -u, v, -v}) {
        DrawWorldLine(projection, capsule.start + side * r, capsule.end + side * r, color, thickness);
    }

    // The end caps: two half circles over each end, across each other.
    for (const auto& [center, outward] : {std::pair{capsule.end, axis}, std::pair{capsule.start, -axis}}) {
        for (const glm::vec3& across : {u, v}) {
            glm::vec3 previous = onCircle(center, across, outward, 0.0f);
            for (int i = 1; i <= kCircleSegments / 2; ++i) {
                const float angle = glm::pi<float>() * static_cast<float>(i) / static_cast<float>(kCircleSegments / 2);
                const glm::vec3 point = onCircle(center, across, outward, angle);
                DrawWorldLine(projection, previous, point, color, thickness);
                previous = point;
            }
        }
    }
}

void ViewportPanel::DrawLights(EditorContext& context, const Projection& projection) const {
    const Frost::GameObject* const selected = context.GetSelectedObject();
    ImDrawList* const drawList = ImGui::GetWindowDrawList();

    for (const std::unique_ptr<Frost::GameObject>& object : context.GetLevel().GetObjects()) {
        const Frost::LightComponent* const light = object->TryGetComponent<Frost::LightComponent>();
        if (light == nullptr) { continue; }

        const bool isSelected = object.get() == selected;
        const Frost::WorldPlacement placement = Frost::ComputeWorldPlacement(context.GetLevel().GetRegistry(), object->GetId());
        const glm::vec3 forward = placement.rotation * glm::vec3(0.0f, 0.0f, -1.0f);

        if (isSelected) { DrawLightReach(projection, *object, placement); }

        // Which way directional and spot lights shine.
        if (light->type != Frost::LightType::Point) {
            DrawWorldLine(projection, placement.position, placement.position + forward * kLightArrowLength,
                          isSelected ? kSelectionColor : LightDisplayColor(*light, 200), 2.0f);
        }

        const std::optional<ImVec2> icon = WorldToScreen(projection, placement.position);
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

void ViewportPanel::DrawCameras(EditorContext& context, const Projection& projection) const {
    const Frost::GameObject* const selected = context.GetSelectedObject();
    ImDrawList* const drawList = ImGui::GetWindowDrawList();

    for (const std::unique_ptr<Frost::GameObject>& object : context.GetLevel().GetObjects()) {
        const Frost::CameraComponent* const cameraComponent = object->TryGetComponent<Frost::CameraComponent>();
        if (cameraComponent == nullptr) { continue; }

        const bool isSelected = object.get() == selected;
        const ImU32 color = isSelected ? kSelectionColor : kCameraColor;

        // The shape of what it sees, cut off a short way out: a pyramid from the camera to a
        // rectangle (at a typical 16:9 aspect), with a notch on the top edge showing which way is up.
        const Frost::WorldPlacement placement = Frost::ComputeWorldPlacement(context.GetLevel().GetRegistry(), object->GetId());
        const Frost::Camera view = cameraComponent->ToCamera(placement.position, placement.rotation);
        const glm::vec3 apex = view.GetPosition();
        const float halfHeight = kCameraFrustumLength * std::tan(view.GetVerticalFov() * 0.5f);
        const float halfWidth = halfHeight * kCameraFrustumAspect;
        const glm::vec3 center = apex + view.GetForward() * kCameraFrustumLength;
        const glm::vec3 right = view.GetRight() * halfWidth;
        const glm::vec3 up = view.GetUp() * halfHeight;

        const std::array<glm::vec3, 4> corners = {center - right - up, center + right - up, center + right + up,
                                                  center - right + up};
        for (std::size_t i = 0; i < corners.size(); ++i) {
            DrawWorldLine(projection, apex, corners[i], color, isSelected ? 2.0f : 1.0f);
            DrawWorldLine(projection, corners[i], corners[(i + 1) % corners.size()], color, isSelected ? 2.0f : 1.0f);
        }
        const glm::vec3 notch = center + up * 1.35f;
        DrawWorldLine(projection, center - right * 0.3f + up, notch, color, 1.0f);
        DrawWorldLine(projection, notch, center + right * 0.3f + up, color, 1.0f);

        const std::optional<ImVec2> icon = WorldToScreen(projection, apex);
        if (!icon.has_value()) { continue; }

        drawList->AddCircleFilled(*icon, kLightIconRadius, IM_COL32(90, 90, 100, 230));
        drawList->AddCircle(*icon, kLightIconRadius, isSelected ? kSelectionColor : IM_COL32(20, 20, 20, 255), 0,
                            isSelected ? 2.5f : 1.5f);
        const ImVec2 textSize = ImGui::CalcTextSize("C");
        drawList->AddText(ImVec2(icon->x - textSize.x * 0.5f, icon->y - textSize.y * 0.5f), IM_COL32(240, 240, 240, 255),
                          "C");
    }
}

void ViewportPanel::DrawLightReach(const Projection& projection, const Frost::GameObject& object,
                                   const Frost::WorldPlacement& placement) const {
    const Frost::LightComponent& light = object.GetComponent<Frost::LightComponent>();
    const glm::vec3& position = placement.position;
    const glm::vec3 forward = glm::normalize(placement.rotation * glm::vec3(0.0f, 0.0f, -1.0f));

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
            constexpr float kRayLength = 3.0f * Frost::kMeter;
            constexpr float kSpread = 40.0f * Frost::kCentimeter;
            for (const glm::vec3& offset : {glm::vec3(0.0f), u * kSpread, -u * kSpread, v * kSpread, -v * kSpread}) {
                DrawWorldLine(projection, position + offset, position + offset + forward * kRayLength, kSelectionColor,
                              1.0f);
            }
            break;
        }
    }
}

void ViewportPanel::DrawCameraInfo(EditorContext& context, const EditorCamera& camera) const {
    const glm::vec3& position = camera.GetPosition();

    char text[160];
    std::snprintf(text, sizeof(text), "Speed %.0f cm/s   Pos %.0f, %.0f, %.0f cm%s", camera.GetMoveSpeed(), position.x,
                  position.y, position.z, camera.IsControlling() ? "" : "   (hold RMB to fly)");

    const ImVec2 at(m_Origin.x + 8.0f, m_Origin.y + 6.0f);
    ImGui::GetWindowDrawList()->AddText(at, IM_COL32(230, 230, 230, 200), text);

    // Unmissable while the physics simulation is running (objects are moving on their own, and will
    // jump back when it stops) or the game is (in its own window).
    ImVec2 line(at.x, at.y + ImGui::GetTextLineHeightWithSpacing());
    if (context.IsSimulating()) {
        ImGui::GetWindowDrawList()->AddText(line, kSimulatingTextColor, "SIMULATING (Alt+S to stop)");
        line.y += ImGui::GetTextLineHeightWithSpacing();
    }
    if (context.IsPlaying()) {
        ImGui::GetWindowDrawList()->AddText(line, kPlayingTextColor, "PLAYING in the game window (F5 to stop)");
    }
}
