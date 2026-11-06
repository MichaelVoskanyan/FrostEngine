#include <Frost/Scene/Components/TransformComponent.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

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

}  // namespace Frost
