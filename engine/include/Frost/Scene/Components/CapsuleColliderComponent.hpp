#pragma once

#include <Frost/Physics/CollisionFilter.hpp>
#include <Frost/Scene/Bounds.hpp>
#include <Frost/Units.hpp>

#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include <algorithm>

namespace Frost {

    // A solid capsule that collides (see PhysicsSystem): a cylinder with a half-sphere on each end,
    // standing along the object's own up (Y) axis. The usual shape for a character: it has no
    // edges to catch on, and slides off steps and corners smoothly.
    //
    // Like a box collider it is defined in the object's own space and follows its transform. A
    // scaled capsule is still a capsule: its height scales with the object's Y scale, and its radius
    // with the larger of its X and Z scales.
    struct CapsuleColliderComponent {
        static constexpr const char* kDisplayName = "Capsule Collider";

        glm::vec3 center{0.0f};  // The capsule's middle, in the object's space (before its scale).
        float radius = 40.0f * kCentimeter;  // Of the cylinder and the end half-spheres.
        float height = 1.8f * kMeter;        // Total, tip to tip (no taller than 2 * radius: a sphere).

        // Its collision layer, and the layers it collides with (see CollisionFilter).
        CollisionFilter filter{};

        // The capsule standing in a box in the object's space (e.g. its model's bounds): as tall as
        // the box, and as wide as its wider horizontal side. On the default layer.
        [[nodiscard]]
        static CapsuleColliderComponent FromBounds(const Bounds& localBounds) noexcept {
            const glm::vec3 size = localBounds.GetSize();
            return CapsuleColliderComponent{.center = localBounds.GetCenter(),
                                            .radius = std::max(size.x, size.z) * 0.5f,
                                            .height = size.y};
        }
    };

}  // namespace Frost
