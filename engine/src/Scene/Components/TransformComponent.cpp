#include <Frost/Scene/Components/TransformComponent.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace Frost {

    glm::mat4 TransformComponent::ToMatrix() const noexcept {
        return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(GetRotation()) *
               glm::scale(glm::mat4(1.0f), scale);
    }

    glm::quat TransformComponent::GetRotation() const noexcept {
        const glm::quat yaw = glm::angleAxis(glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::quat pitch = glm::angleAxis(glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::quat roll = glm::angleAxis(glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        return yaw * pitch * roll;
    }

    glm::vec3 TransformComponent::GetForward() const noexcept {
        return glm::normalize(GetRotation() * glm::vec3(0.0f, 0.0f, -1.0f));
    }

    void TransformComponent::SetFromMatrix(const glm::mat4& matrix) noexcept {
        position = glm::vec3(matrix[3]);

        glm::mat3 axes(matrix);
        for (glm::length_t i = 0; i < 3; ++i) {
            scale[i] = glm::length(axes[i]);
            if (scale[i] > 0.0f) { axes[i] /= scale[i]; }
        }
        if (glm::determinant(axes) < 0.0f) {
            scale.x = -scale.x;
            axes[0] = -axes[0];
        }

        // R = Ry(yaw) * Rx(pitch) * Rz(roll) has -sin(pitch) in row 1, column 2 (axes[column][row]).
        const float sinPitch = std::clamp(-axes[2][1], -1.0f, 1.0f);
        const float pitch = std::asin(sinPitch);
        float yaw = 0.0f;
        float roll = 0.0f;
        if (std::abs(sinPitch) < 0.9999f) {
            yaw = std::atan2(axes[2][0], axes[2][2]);
            roll = std::atan2(axes[0][1], axes[1][1]);
        }
        else {
            yaw = std::atan2(-axes[0][2], axes[0][0]);  // Gimbal lock: all of the turn as yaw.
        }
        rotation = glm::degrees(glm::vec3(pitch, yaw, roll));
    }

}  // namespace Frost
