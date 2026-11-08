#pragma once

// What the physics queries (raycasts, shape casts, overlaps) share: which colliders they see, and
// how hits are collected and ordered. Private to the engine.

#include <Frost/Physics/Collision.hpp>
#include <Frost/Physics/Raycast.hpp>
#include <Frost/Scene/Components/BoxColliderComponent.hpp>
#include <Frost/Scene/Components/CapsuleColliderComponent.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>
#include <Frost/Scene/Hierarchy.hpp>
#include <Frost/Scene/Registry.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Frost::Detail {

    // Directions shorter than this (squared) have no usable direction to normalize to.
    inline constexpr float kMinDirectionLengthSquared = 1.0e-12f;

    [[nodiscard]]
    inline bool IsFinite(const glm::vec3& vector) noexcept {
        return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
    }

    // `direction` at unit length, or nothing if it has no usable direction (zero, or non-finite).
    [[nodiscard]]
    inline std::optional<glm::vec3> NormalizeDirection(const glm::vec3& direction) noexcept {
        const float lengthSquared = glm::dot(direction, direction);
        if (!(lengthSquared > kMinDirectionLengthSquared) || !std::isfinite(lengthSquared)) { return std::nullopt; }
        return direction / std::sqrt(lengthSquared);
    }

    // The ray with a unit-length direction, or nothing if it has no usable direction or origin.
    [[nodiscard]]
    inline std::optional<Ray> NormalizeRay(const Ray& ray) noexcept {
        const std::optional<glm::vec3> direction = NormalizeDirection(ray.direction);
        if (!direction.has_value() || !IsFinite(ray.origin)) { return std::nullopt; }
        return Ray{.origin = ray.origin, .direction = *direction};
    }

    // The collider of one object, as the PhysicsSystem sees it (its box if it has both; none for an
    // attached object), in the world, with its layer.
    struct WorldCollider {
        CollisionShape shape;
        CollisionFilter filter;
    };

    [[nodiscard]]
    inline std::optional<WorldCollider> FindWorldCollider(const Registry& registry, const EntityId entity) {
        const TransformComponent* const transform = registry.TryGet<TransformComponent>(entity);
        if (transform == nullptr || IsAttached(registry, entity)) { return std::nullopt; }
        if (const BoxColliderComponent* const box = registry.TryGet<BoxColliderComponent>(entity)) {
            return WorldCollider{.shape = MakeWorldBox(*transform, *box), .filter = box->filter};
        }
        if (const CapsuleColliderComponent* const capsule = registry.TryGet<CapsuleColliderComponent>(entity)) {
            return WorldCollider{.shape = MakeWorldCapsule(*transform, *capsule), .filter = capsule->filter};
        }
        return std::nullopt;
    }

    // Calls `visit(entity, shape)` for the collider of every object that has one and that `filter`
    // accepts, as the PhysicsSystem sees them: one shape per object (its box if it has both),
    // attached objects left out.
    template <typename Visit>
    void ForEachCollider(const Registry& registry, const RaycastFilter& filter, Visit&& visit) {
        if (filter.layers == kNoCollisionLayers) { return; }

        registry.Each<TransformComponent, BoxColliderComponent>(
                [&](const EntityId entity, const TransformComponent& transform, const BoxColliderComponent& collider) {
                    if (!filter.Accepts(entity, collider.filter.layer) || IsAttached(registry, entity)) { return; }
                    visit(entity, CollisionShape(MakeWorldBox(transform, collider)));
                });
        registry.Each<TransformComponent, CapsuleColliderComponent>(
                [&](const EntityId entity, const TransformComponent& transform, const CapsuleColliderComponent& collider) {
                    // One collider per object: a box, if it also has one, was taken above.
                    if (registry.Has<BoxColliderComponent>(entity) || !filter.Accepts(entity, collider.filter.layer) ||
                        IsAttached(registry, entity)) {
                        return;
                    }
                    visit(entity, CollisionShape(MakeWorldCapsule(transform, collider)));
                });
    }

    // The order hits are reported in: nearest first, ties by entity id (so the order does not
    // depend on how the registry happens to store the components).
    [[nodiscard]]
    inline bool IsBefore(const RaycastHit& a, const RaycastHit& b) noexcept {
        return a.distance != b.distance ? a.distance < b.distance : a.entity < b.entity;
    }

    // The ...All half of a cast: `cast(shape, maxDistance)` gives where the cast hits one collider
    // (std::optional<RaycastHit>; its entity is filled in here). Every hit, nearest first.
    template <typename Cast>
    [[nodiscard]]
    std::vector<RaycastHit> CollectHits(const Registry& registry, const RaycastFilter& filter, const float maxDistance,
                                        Cast&& cast) {
        std::vector<RaycastHit> hits;
        ForEachCollider(registry, filter, [&](const EntityId entity, const CollisionShape& shape) {
            if (std::optional<RaycastHit> hit = cast(shape, maxDistance)) {
                hit->entity = entity;
                hits.push_back(*hit);
            }
        });
        std::sort(hits.begin(), hits.end(), IsBefore);
        return hits;
    }

    // The ...Closest half: the first hit CollectHits would give, without collecting the rest.
    template <typename Cast>
    [[nodiscard]]
    std::optional<RaycastHit> FindClosestHit(const Registry& registry, const RaycastFilter& filter,
                                             const float maxDistance, Cast&& cast) {
        std::optional<RaycastHit> nearest;
        ForEachCollider(registry, filter, [&](const EntityId entity, const CollisionShape& shape) {
            // Nothing further than the nearest hit so far can win (an equally near one still can,
            // by entity id).
            const float reach = nearest.has_value() ? nearest->distance : maxDistance;
            if (std::optional<RaycastHit> hit = cast(shape, reach)) {
                hit->entity = entity;
                if (!nearest.has_value() || IsBefore(*hit, *nearest)) { nearest = *hit; }
            }
        });
        return nearest;
    }

}  // namespace Frost::Detail
