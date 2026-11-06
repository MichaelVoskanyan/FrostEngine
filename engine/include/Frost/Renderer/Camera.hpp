#pragma once

#include <Frost/Units.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace Frost {

    // A perspective camera: a world-space position and orientation plus projection parameters.
    // Plain value type with no input handling or update logic; controllers (the editor's fly
    // camera, a game's player camera) own one and drive it, and hand it to Renderer::SetCamera.
    //
    // Conventions match the rest of the renderer: right-handed world, +Y up, and the camera looks
    // down its local -Z with +X to its right. The projection maps depth to Vulkan's [0, 1] range
    // reversed (near plane -> 1, far plane -> 0; see VulkanPipeline for why) and keeps clip-space
    // +Y up, which the renderer's flipped viewport relies on.
    //
    // The aspect ratio is deliberately not stored: it belongs to whatever the camera renders into,
    // so the renderer supplies it (from its actual target extent) when it builds the projection.
    class Camera final {
    public:
        // verticalFov in radians, in (0, pi); 0 < nearClip < farClip. Throws std::invalid_argument
        // otherwise.
        void SetPerspective(float verticalFov, float nearClip, float farClip);

        void SetPosition(const glm::vec3& position) noexcept;

        // Rotation from camera-local to world space; normalized on the way in.
        void SetOrientation(const glm::quat& orientation) noexcept;

        [[nodiscard]]
        const glm::vec3& GetPosition() const noexcept;

        [[nodiscard]]
        const glm::quat& GetOrientation() const noexcept;

        // World-space basis of the camera (unit length).
        [[nodiscard]]
        glm::vec3 GetForward() const noexcept;

        [[nodiscard]]
        glm::vec3 GetRight() const noexcept;

        [[nodiscard]]
        glm::vec3 GetUp() const noexcept;

        [[nodiscard]]
        float GetVerticalFov() const noexcept;

        [[nodiscard]]
        float GetNearClip() const noexcept;

        [[nodiscard]]
        float GetFarClip() const noexcept;

        // World -> view space.
        [[nodiscard]]
        glm::mat4 GetViewMatrix() const noexcept;

        // View -> clip space for a target of the given width/height ratio. A non-positive ratio
        // (e.g. a zero-area target) falls back to 1 rather than producing a degenerate matrix.
        [[nodiscard]]
        glm::mat4 GetProjectionMatrix(float aspectRatio) const noexcept;

    private:
        glm::vec3 m_Position{0.0f};
        glm::quat m_Orientation{1.0f, 0.0f, 0.0f, 0.0f};  // (w, x, y, z): identity, looking down -Z.

        float m_VerticalFov = 1.0471976f;  // 60 degrees.
        float m_NearClip = 5.0f * kCentimeter;
        float m_FarClip = 1.0f * kKilometer;
    };

}  // namespace Frost
