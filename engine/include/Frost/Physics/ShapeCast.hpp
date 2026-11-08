#pragma once

#include <Frost/Physics/Collision.hpp>
#include <Frost/Physics/Raycast.hpp>
#include <Frost/Scene/Ray.hpp>
#include <Frost/Scene/Registry.hpp>

#include <glm/vec3.hpp>

#include <optional>
#include <vector>

namespace Frost {

    // Shape casts: a sphere or capsule moved along a straight line through the physics world, and
    // where it comes to touch colliders on the way (e.g. whether a character fits through a gap,
    // where a thick projectile lands, a ground check with some width).
    //
    // They see the same colliders as raycasts, with the same filter (see RaycastFilter and
    // RaycastAll), and give the same RaycastHit, where:
    //   - distance is how far the shape travelled before touching the collider (in centimeters,
    //     along the normalized direction);
    //   - point is where they touch, on the collider's surface;
    //   - normal is the collider's surface normal there (for a box's edge or corner, the direction
    //     from it towards the moving shape), unit length, facing the moving shape.
    //
    // As with raycasts, a collider the shape starts overlapping is not hit (touching alone is not
    // overlapping), so a character's own capsule cast from where it stands does not hit the
    // character. Only moving into a collider hits it: a shape that slides along a surface it
    // touches (a character cast forward along the ground it stands on), or passes a collider's
    // edge with no room to spare, does not hit it. A radius of 0 makes a sphere cast a raycast. A zero or non-finite direction, or a
    // negative radius, hits nothing.

    // Every collider a sphere of `radius` hits moving from `ray.origin` along `ray.direction`,
    // within `maxDistance`, nearest first (one hit per object: where it first touches it).
    [[nodiscard]]
    std::vector<RaycastHit> SphereCastAll(const Registry& registry, const Ray& ray, float radius,
                                          float maxDistance = kDefaultRaycastDistance, const RaycastFilter& filter = {});

    // The first collider the sphere touches, if any (the first of SphereCastAll).
    [[nodiscard]]
    std::optional<RaycastHit> SphereCastClosest(const Registry& registry, const Ray& ray, float radius,
                                                float maxDistance = kDefaultRaycastDistance,
                                                const RaycastFilter& filter = {});

    // Every collider `capsule` (in world space, e.g. MakeWorldCapsule of a collider) hits moving
    // along `direction`, within `maxDistance`, nearest first.
    [[nodiscard]]
    std::vector<RaycastHit> CapsuleCastAll(const Registry& registry, const Capsule& capsule, const glm::vec3& direction,
                                           float maxDistance = kDefaultRaycastDistance, const RaycastFilter& filter = {});

    // The first collider the capsule touches, if any (the first of CapsuleCastAll).
    [[nodiscard]]
    std::optional<RaycastHit> CapsuleCastClosest(const Registry& registry, const Capsule& capsule,
                                                 const glm::vec3& direction, float maxDistance = kDefaultRaycastDistance,
                                                 const RaycastFilter& filter = {});

    // --- Against single shapes -------------------------------------------------------------------

    // Where a moving shape first touches a collider's shape (no object, so no entity).
    struct ShapeCastHit {
        float distance = 0.0f;               // How far it travelled, along the normalized direction.
        glm::vec3 point{0.0f};               // Where they touch, on the target's surface.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};  // The target's surface normal there (unit length).
    };

    // Where `capsule` (a sphere, if its ends are the same point), moving along `direction`, first
    // touches `target`, if it does within [0, maxDistance]. Nothing if it starts overlapping it.
    [[nodiscard]]
    std::optional<ShapeCastHit> CastCapsule(const Capsule& capsule, const glm::vec3& direction, float maxDistance,
                                            const OrientedBox& target) noexcept;

    [[nodiscard]]
    std::optional<ShapeCastHit> CastCapsule(const Capsule& capsule, const glm::vec3& direction, float maxDistance,
                                            const Capsule& target) noexcept;

    [[nodiscard]]
    std::optional<ShapeCastHit> CastCapsule(const Capsule& capsule, const glm::vec3& direction, float maxDistance,
                                            const CollisionShape& target) noexcept;

}  // namespace Frost
