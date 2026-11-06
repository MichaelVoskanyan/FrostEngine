#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace Frost {

    // Where an entity is: position, rotation and scale, composed as translate * rotate * scale.
    // Every object in a level has one (see GameObject).
    //
    // Rotation is stored as Euler angles in degrees, because that is what people edit and what
    // round-trips through a level file without drift: x = pitch (about X), y = yaw (about Y),
    // z = roll (about Z), applied roll first, then pitch, then yaw (R = Ry * Rx * Rz), so yaw is
    // always about the world's up axis.
    struct TransformComponent {
        static constexpr const char* kDisplayName = "Transform";

        glm::vec3 position{0.0f};
        glm::vec3 rotation{0.0f};
        glm::vec3 scale{1.0f};

        [[nodiscard]]
        glm::mat4 ToMatrix() const noexcept;

        // The rotation as a quaternion (R = Ry * Rx * Rz, see above).
        [[nodiscard]]
        glm::quat GetRotation() const noexcept;

        // The object's forward direction in world space: its local -Z rotated (unit length). Lights
        // shine this way, as the camera looks this way.
        [[nodiscard]]
        glm::vec3 GetForward() const noexcept;
    };

}  // namespace Frost
