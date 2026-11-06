#pragma once

#include <Frost/Physics/Collision.hpp>
#include <Frost/Scene/Level.hpp>

#include <imgui.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <optional>

class EditorContext;
class EditorCamera;

// The "Viewport" window: the 3D scene. The window itself is transparent; the renderer draws the
// scene into exactly its area (Renderer::SetSceneViewport), so docked panels around it never
// cover or distort it. Handles what happens in that area:
//   - left click selects the light/camera icon or object under the cursor (or clears the selection);
//   - right button flies the camera (EditorCamera; hover state is exposed for that);
//   - models dragged in from the Asset Library are placed where they are dropped, and so are
//     lights dragged from its Lights tab;
//   - lights, which have no geometry, are drawn as icons (with their direction), and the selected
//     light's reach is shown (range sphere, spot cone, sun rays); cameras likewise, as an icon and
//     the shape of their view;
//   - box colliders are drawn as wireframes (immovable ones green, simulated bodies' blue), so
//     invisible ones such as a ground box can be seen;
//   - the selected object's bounds are outlined, and camera info (and whether the physics
//     simulation or the game is running) is shown in a corner.
class ViewportPanel final {
public:
    static constexpr const char* kWindowName = "Viewport";

    void Draw(EditorContext& context, const EditorCamera& camera);

    // Whether the cursor was over the scene area on the last Draw() (and no UI was in the way).
    [[nodiscard]]
    bool IsHovered() const noexcept;

    // The world-space ray through a point of the scene area (ImGui screen coordinates).
    [[nodiscard]]
    Frost::Ray ScreenPointToRay(const EditorCamera& camera, const ImVec2& screenPoint) const;

    // Whether box colliders are drawn (on by default).
    [[nodiscard]]
    bool GetShowColliders() const noexcept;

    void SetShowColliders(bool show) noexcept;

private:
    // The camera's matrices for the current scene area, for drawing overlays in world space.
    struct Projection {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        float nearClip = 0.05f;
    };

    [[nodiscard]]
    Projection MakeProjection(const EditorCamera& camera) const;

    // Screen position of a world point, or nullopt if it is behind the camera.
    [[nodiscard]]
    std::optional<ImVec2> WorldToScreen(const Projection& projection, const glm::vec3& world) const;

    // A world-space line/circle drawn over the scene, clipped against the near plane.
    void DrawWorldLine(const Projection& projection, const glm::vec3& a, const glm::vec3& b, ImU32 color,
                       float thickness = 1.5f) const;
    void DrawWorldCircle(const Projection& projection, const glm::vec3& center, const glm::vec3& axisU,
                         const glm::vec3& axisV, float radius, ImU32 color) const;

    // Where a dropped model or light should go: on the surface under the cursor, else on the
    // ground plane (y = 0), else a few units in front of the camera.
    [[nodiscard]]
    glm::vec3 FindDropPoint(EditorContext& context, const Frost::Ray& ray) const;

    // The light or camera whose icon is under `screenPoint` (the nearest to the camera if several),
    // if any.
    [[nodiscard]]
    Frost::GameObject* PickIcon(EditorContext& context, const Projection& projection, const ImVec2& screenPoint) const;

    void DrawSelectionBounds(EditorContext& context, const Projection& projection) const;
    void DrawColliders(EditorContext& context, const Projection& projection) const;
    void DrawCapsule(const Projection& projection, const Frost::Capsule& capsule, ImU32 color, float thickness) const;
    void DrawLights(EditorContext& context, const Projection& projection) const;
    void DrawCameras(EditorContext& context, const Projection& projection) const;
    void DrawLightReach(const Projection& projection, const Frost::GameObject& object) const;
    void DrawCameraInfo(EditorContext& context, const EditorCamera& camera) const;

    ImVec2 m_Origin{0.0f, 0.0f};  // Top-left of the scene area, ImGui screen coordinates.
    ImVec2 m_Size{0.0f, 0.0f};
    bool m_Hovered = false;
    bool m_ShowColliders = true;
};
