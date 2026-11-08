#pragma once

#include <Frost/Physics/CollisionFilter.hpp>
#include <Frost/Scene/Bounds.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>

namespace Frost {

    // A solid box that other objects collide with (see PhysicsSystem). It is defined in the
    // object's own space, so it moves, rotates and scales with the object's transform.
    //
    // On its own (or with a rigid body that is not simulated) a collider is an immovable obstacle,
    // such as the ground; on an object whose rigid body is simulated it is that body's shape.
    struct BoxColliderComponent {
        static constexpr const char* kDisplayName = "Box Collider";

        glm::vec3 center{0.0f};  // The box's center, in the object's space (before its scale).
        glm::vec3 size{1.0f * kMeter};  // Full edge lengths, in the object's space (before its scale).

        // Its collision layer, and the layers it collides with (see CollisionFilter).
        CollisionFilter filter{};

        // A collider exactly enclosing a box in the object's space (e.g. its model's bounds), on
        // the default layer.
        [[nodiscard]]
        static BoxColliderComponent FromBounds(const Bounds& localBounds) noexcept {
            return BoxColliderComponent{.center = localBounds.GetCenter(), .size = localBounds.GetSize()};
        }
    };

}  // namespace Frost
