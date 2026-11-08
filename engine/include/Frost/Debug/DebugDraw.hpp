#pragma once

#include <Frost/Physics/Collision.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Frost {

    class Camera;

    // Debug drawing colors: sRGB red, green, blue and alpha, in [0, 1] (as picked in a paint
    // program).
    using DebugColor = glm::vec4;

    namespace DebugColors {
        inline constexpr DebugColor kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        inline constexpr DebugColor kGray{0.6f, 0.6f, 0.6f, 0.8f};
        inline constexpr DebugColor kRed{1.0f, 0.25f, 0.2f, 1.0f};
        inline constexpr DebugColor kGreen{0.3f, 0.95f, 0.4f, 1.0f};
        inline constexpr DebugColor kBlue{0.35f, 0.6f, 1.0f, 1.0f};
        inline constexpr DebugColor kYellow{1.0f, 0.9f, 0.25f, 1.0f};
        inline constexpr DebugColor kOrange{1.0f, 0.6f, 0.15f, 1.0f};
        inline constexpr DebugColor kCyan{0.3f, 0.9f, 1.0f, 1.0f};
    }  // namespace DebugColors

    // Immediate-mode debug drawing: lines, shapes and labels in world space, collected over a frame
    // and drawn on top of the scene (Render), then cleared. For seeing what code does in the world
    // (where a raycast goes, what it hit), not for anything a player should see.
    //
    // Everything is drawn over the scene, including what is behind walls (there is no depth test).
    class DebugDraw final {
    public:
        struct Line {
            glm::vec3 from{0.0f};
            glm::vec3 to{0.0f};
            DebugColor color{1.0f};
            float thickness = 1.0f;  // Pixels.
        };

        // Text at a point in the world.
        struct Label {
            glm::vec3 position{0.0f};
            std::string text;
            DebugColor color{1.0f};
        };

        // Segments circles and spheres are drawn with.
        static constexpr int kCircleSegments = 32;

        void DrawLine(const glm::vec3& from, const glm::vec3& to, const DebugColor& color, float thickness = 1.0f);

        // A line with an arrowhead at `to`.
        void DrawArrow(const glm::vec3& from, const glm::vec3& to, const DebugColor& color, float thickness = 1.0f);

        // A small 3D cross (three lines of `size`) at `position`.
        void DrawPoint(const glm::vec3& position, float size, const DebugColor& color);

        // A circle around `center` in the plane facing `normal`.
        void DrawCircle(const glm::vec3& center, const glm::vec3& normal, float radius, const DebugColor& color);

        // A wire sphere: its three great circles along the world axes.
        void DrawSphere(const glm::vec3& center, float radius, const DebugColor& color);

        // A wire capsule: rings around its ends, four lines along its sides and the outlines of its
        // end caps.
        void DrawCapsule(const Capsule& capsule, const DebugColor& color);

        // A wire box: its 12 edges.
        void DrawBox(const OrientedBox& box, const DebugColor& color);

        // Any collider shape.
        void DrawShape(const CollisionShape& shape, const DebugColor& color);

        void DrawLabel(const glm::vec3& position, std::string text, const DebugColor& color = DebugColors::kWhite);

        // Forgets everything drawn (call once the frame is rendered).
        void Clear() noexcept;

        [[nodiscard]]
        bool IsEmpty() const noexcept;

        [[nodiscard]]
        const std::vector<Line>& GetLines() const noexcept;

        [[nodiscard]]
        const std::vector<Label>& GetLabels() const noexcept;

        // Draws everything onto the screen, over the scene, as seen by `camera` (the one the scene
        // is rendered with), through Dear ImGui's background draw list: the engine must run with
        // EngineSpecification::enableImGui, and this must be called while an ImGui frame is open
        // (e.g. from Application::OnImGuiRender). The scene is assumed to fill the window.
        void Render(const Camera& camera) const;

    private:
        std::vector<Line> m_Lines;
        std::vector<Label> m_Labels;
    };

    // Where a world point lands on a screen of `screenSize` pixels (origin top left), as seen by a
    // camera with these view and projection matrices, or nothing if it is behind the near plane
    // (`nearClip` in front of the camera). What DebugDraw::Render places things with.
    [[nodiscard]]
    std::optional<glm::vec2> ProjectToScreen(const glm::mat4& view, const glm::mat4& projection, float nearClip,
                                             const glm::vec2& screenSize, const glm::vec3& point) noexcept;

    // The part of the line from `from` to `to` that is in front of the near plane, on the screen (as
    // ProjectToScreen), or nothing if it is wholly behind it.
    struct ScreenLine {
        glm::vec2 from{0.0f};
        glm::vec2 to{0.0f};
    };

    [[nodiscard]]
    std::optional<ScreenLine> ProjectLineToScreen(const glm::mat4& view, const glm::mat4& projection, float nearClip,
                                                  const glm::vec2& screenSize, const glm::vec3& from,
                                                  const glm::vec3& to) noexcept;

}  // namespace Frost
