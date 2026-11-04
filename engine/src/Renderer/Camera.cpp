#include <Frost/Renderer/Camera.hpp>

#include <glm/ext/scalar_constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <stdexcept>

namespace Frost {

    void Camera::SetPerspective(const float verticalFov, const float nearClip, const float farClip) {
        if (!(verticalFov > 0.0f && verticalFov < glm::pi<float>())) {
            throw std::invalid_argument("Camera: vertical field of view must be in (0, pi) radians.");
        }
        if (!(nearClip > 0.0f && farClip > nearClip)) {
            throw std::invalid_argument("Camera: clip planes must satisfy 0 < near < far.");
        }

        m_VerticalFov = verticalFov;
        m_NearClip = nearClip;
        m_FarClip = farClip;
    }

    void Camera::SetPosition(const glm::vec3& position) noexcept { m_Position = position; }

    void Camera::SetOrientation(const glm::quat& orientation) noexcept { m_Orientation = glm::normalize(orientation); }

    const glm::vec3& Camera::GetPosition() const noexcept { return m_Position; }

    const glm::quat& Camera::GetOrientation() const noexcept { return m_Orientation; }

    glm::vec3 Camera::GetForward() const noexcept { return m_Orientation * glm::vec3(0.0f, 0.0f, -1.0f); }

    glm::vec3 Camera::GetRight() const noexcept { return m_Orientation * glm::vec3(1.0f, 0.0f, 0.0f); }

    glm::vec3 Camera::GetUp() const noexcept { return m_Orientation * glm::vec3(0.0f, 1.0f, 0.0f); }

    float Camera::GetVerticalFov() const noexcept { return m_VerticalFov; }

    float Camera::GetNearClip() const noexcept { return m_NearClip; }

    float Camera::GetFarClip() const noexcept { return m_FarClip; }

    glm::mat4 Camera::GetViewMatrix() const noexcept {
        // Inverse of the camera's rigid transform (translate * rotate): the inverse rotation (the
        // conjugate of a unit quaternion) applied after undoing the translation.
        return glm::mat4_cast(glm::conjugate(m_Orientation)) * glm::translate(glm::mat4(1.0f), -m_Position);
    }

    glm::mat4 Camera::GetProjectionMatrix(const float aspectRatio) const noexcept {
        const float aspect = aspectRatio > 0.0f ? aspectRatio : 1.0f;

        // Right-handed, depth to [0, 1] (Vulkan), reversed-Z: passing the planes swapped maps the
        // near plane to 1 and the far plane to 0, matching the pipeline's GREATER_OR_EQUAL test.
        // No Y flip here: the renderer flips the viewport instead, so clip-space +Y stays up just
        // like world space.
        return glm::perspectiveRH_ZO(m_VerticalFov, aspect, m_FarClip, m_NearClip);
    }

}  // namespace Frost
