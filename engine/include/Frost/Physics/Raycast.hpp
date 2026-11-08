#pragma once

#include <Frost/Physics/Collision.hpp>
#include <Frost/Physics/CollisionFilter.hpp>
#include <Frost/Scene/Ray.hpp>
#include <Frost/Scene/Registry.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>

#include <optional>
#include <vector>

namespace Frost {

    // How far a raycast (or shape cast, see ShapeCast.hpp) reaches unless told otherwise.
    inline constexpr float kDefaultRaycastDistance = 10.0f * kKilometer;

    // Where a ray meets the surface of an object. Shape casts (ShapeCast.hpp) give the same, with
    // `distance` how far the shape travelled and `point` where it touches the surface.
    struct RaycastHit {
        EntityId entity = 0;    // The object hit (in a level, its ObjectId).
        float distance = 0.0f;  // From the ray's origin to `point`, in world units (centimeters).
        glm::vec3 point{0.0f};  // World space.
        // The surface's normal at `point`: world space, unit length, facing back towards where the
        // ray came from (for a collider, its outward normal there).
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
    };

    // Which colliders a physics query may hit (or, for an overlap query, find): raycasts, shape
    // casts (ShapeCast.hpp) and overlaps (Overlap.hpp) all take one. The default accepts every one.
    struct RaycastFilter {
        // Only colliders on these layers are hit (see CollisionFilter; a collider's own mask does
        // not matter to queries, only its layer). E.g. CollisionLayerBit(2) for layer 2 only, or
        // kAllCollisionLayers & ~CollisionLayerBit(5) for every layer but 5.
        CollisionLayerMask layers = kAllCollisionLayers;

        // These objects are never hit: e.g. the character firing a shot from its weapon's muzzle,
        // which can be outside the character's own capsule. (Objects attached to others, such as
        // the weapon itself, are never hit anyway.)
        std::vector<EntityId> ignoredEntities;

        // Whether the filter lets a query hit the collider of `entity`, on `layer`.
        [[nodiscard]]
        bool Accepts(EntityId entity, int layer) const noexcept;
    };

    // --- Against colliders (the physics world) ---------------------------------------------------
    //
    // These test the same colliders the PhysicsSystem collides with: every object's box or capsule
    // collider (an object with both uses its box), whether or not it has a rigid body, and whether
    // or not it is drawn, less those `filter` rules out. Objects attached to another object are
    // left out, as physics leaves them out. Meshes are not tested (see Level::Raycast for the
    // editor's mesh picking).
    //
    // The ray's direction need not be unit length: it is normalized, so distances are always in
    // world units (centimeters). A zero or non-finite direction hits nothing.
    //
    // A collider the ray starts inside is not hit (so a ray cast from inside a character's own
    // capsule, e.g. from its camera, does not hit the character). A ray starting exactly on a
    // surface and heading into it hits it at distance 0.

    // Every collider the ray hits within `maxDistance`, nearest first (one hit per object: where the
    // ray enters its collider). Equally distant hits are ordered by entity id.
    [[nodiscard]]
    std::vector<RaycastHit> RaycastAll(const Registry& registry, const Ray& ray,
                                       float maxDistance = kDefaultRaycastDistance, const RaycastFilter& filter = {});

    // The nearest collider the ray hits within `maxDistance`, if any (the first of RaycastAll,
    // without collecting the rest).
    [[nodiscard]]
    std::optional<RaycastHit> RaycastClosest(const Registry& registry, const Ray& ray,
                                             float maxDistance = kDefaultRaycastDistance,
                                             const RaycastFilter& filter = {});

    // --- Against single shapes -------------------------------------------------------------------

    // Where a ray enters a shape, for the raycasts above (no object, so no entity).
    struct RayShapeHit {
        float distance = 0.0f;               // World units along the (normalized) ray direction.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};  // The shape's outward normal there (unit length).
    };

    // Where the ray first enters the shape, if it does within [0, maxDistance]. Nothing if the ray
    // starts inside the shape (see above).
    [[nodiscard]]
    std::optional<RayShapeHit> IntersectRay(const Ray& ray, const OrientedBox& box, float maxDistance) noexcept;

    [[nodiscard]]
    std::optional<RayShapeHit> IntersectRay(const Ray& ray, const Capsule& capsule, float maxDistance) noexcept;

    [[nodiscard]]
    std::optional<RayShapeHit> IntersectRay(const Ray& ray, const CollisionShape& shape, float maxDistance) noexcept;

}  // namespace Frost
