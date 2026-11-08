#include <Frost/Physics/Raycast.hpp>

#include "ColliderQuery.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

    // A ray moving less than this across a box's slab (per unit travelled) runs parallel to it;
    // likewise across a capsule's axis (squared).
    constexpr float kParallelEpsilon = 1.0e-8f;

    // Capsule core segments shorter than this (squared) are points: the capsule is a sphere.
    constexpr float kMinSegmentLengthSquared = 1.0e-8f * Frost::kCentimeter * Frost::kCentimeter;

    // `vector` at unit length, or `fallback` if it is too short to have a direction.
    [[nodiscard]]
    glm::vec3 NormalizeOr(const glm::vec3& vector, const glm::vec3& fallback) noexcept {
        const float lengthSquared = glm::dot(vector, vector);
        return lengthSquared > 0.0f && std::isfinite(lengthSquared) ? vector / std::sqrt(lengthSquared) : fallback;
    }

    [[nodiscard]]
    glm::vec3 ClosestPointOnSegment(const glm::vec3& point, const glm::vec3& start, const glm::vec3& end) noexcept {
        const glm::vec3 span = end - start;
        const float lengthSquared = glm::dot(span, span);
        if (lengthSquared <= kMinSegmentLengthSquared) { return start; }
        return start + span * glm::clamp(glm::dot(point - start, span) / lengthSquared, 0.0f, 1.0f);
    }

    // How far along a unit-direction ray it enters a sphere (negative: behind the origin), if its
    // line meets the sphere. The discriminant is found from the squared distance between the line
    // and the center, not from |origin - center|^2 - radius^2: in centimeters, a ray starting a few
    // kilometers away would otherwise lose most of a small sphere to float rounding (Haines et al.,
    // "Precision Improvements for Ray/Sphere Intersection", Ray Tracing Gems, 2019).
    [[nodiscard]]
    std::optional<float> EnterSphere(const glm::vec3& offset, const glm::vec3& direction, const float radius) noexcept {
        // offset: from the sphere's center to the ray's origin.
        const float along = glm::dot(offset, direction);
        const glm::vec3 across = offset - direction * along;  // From the center to the line's closest point.
        const float discriminant = radius * radius - glm::dot(across, across);
        if (!(discriminant >= 0.0f)) { return std::nullopt; }
        return -along - std::sqrt(discriminant);
    }

    // A (unit) ray's hit on one collider, for the registry-level raycasts (the entity is filled in
    // by the caller).
    [[nodiscard]]
    std::optional<Frost::RaycastHit> CastRay(const Frost::Ray& unitRay, const Frost::CollisionShape& shape,
                                             const float maxDistance) noexcept {
        const std::optional<Frost::RayShapeHit> hit = Frost::IntersectRay(unitRay, shape, maxDistance);
        if (!hit.has_value()) { return std::nullopt; }
        return Frost::RaycastHit{.entity = 0,
                                 .distance = hit->distance,
                                 .point = unitRay.origin + unitRay.direction * hit->distance,
                                 .normal = hit->normal};
    }

}  // namespace

namespace Frost {

    std::optional<RayShapeHit> IntersectRay(const Ray& ray, const OrientedBox& box, const float maxDistance) noexcept {
        const std::optional<Ray> unit = Detail::NormalizeRay(ray);
        if (!unit.has_value() || !(maxDistance >= 0.0f)) { return std::nullopt; }

        // Slab test in the box's own axes: the ray is inside the box between entering the last of
        // its three slabs and leaving the first.
        const glm::vec3 offset = unit->origin - box.center;
        float entry = -std::numeric_limits<float>::infinity();
        float exit = std::numeric_limits<float>::infinity();
        glm::vec3 entryNormal{0.0f};

        for (int i = 0; i < 3; ++i) {
            const glm::vec3& axis = box.axes[i];
            const float start = glm::dot(offset, axis);  // Where the origin is along this axis.
            const float speed = glm::dot(unit->direction, axis);
            const float halfExtent = box.halfExtents[i];

            if (std::abs(speed) < kParallelEpsilon) {
                if (std::abs(start) > halfExtent) { return std::nullopt; }  // Parallel, outside the slab.
                continue;  // Parallel, inside the slab all along.
            }

            // The face of this slab the ray comes in through, and the one it leaves through.
            const float nearFace = speed > 0.0f ? -halfExtent : halfExtent;
            const float enterSlab = (nearFace - start) / speed;
            const float leaveSlab = (-nearFace - start) / speed;
            if (enterSlab > entry) {
                entry = enterSlab;
                entryNormal = speed > 0.0f ? -axis : axis;  // The near face's outward normal.
            }
            exit = std::min(exit, leaveSlab);
        }

        // Misses (also NaN from a broken transform), or enters it past maxDistance. An entry behind
        // the origin means the ray starts inside the box (or, if it also left it there, that the
        // box is behind the ray): either way, not a hit.
        if (!(entry <= exit) || !(entry >= 0.0f) || entry > maxDistance) { return std::nullopt; }
        return RayShapeHit{.distance = entry > 0.0f ? entry : 0.0f, .normal = entryNormal};
    }

    std::optional<RayShapeHit> IntersectRay(const Ray& ray, const Capsule& capsule, const float maxDistance) noexcept {
        const std::optional<Ray> unit = Detail::NormalizeRay(ray);
        if (!unit.has_value() || !(maxDistance >= 0.0f) || !(capsule.radius > 0.0f)) { return std::nullopt; }

        const glm::vec3& origin = unit->origin;
        const glm::vec3& direction = unit->direction;
        const float radius = capsule.radius;

        // Starting inside: within the radius of the core segment (exactly on the surface is outside).
        const glm::vec3 fromCore = origin - ClosestPointOnSegment(origin, capsule.start, capsule.end);
        if (glm::dot(fromCore, fromCore) < radius * radius) { return std::nullopt; }

        // The capsule is the union of a cylinder around the core segment and a sphere on each of
        // its ends, so (starting outside all of them) the ray enters it where it enters the first
        // of those it meets.
        std::optional<RayShapeHit> nearest;
        const auto keep = [&](const float distance, const glm::vec3& outward) {
            if (!(distance >= 0.0f) || distance > maxDistance) { return; }
            if (nearest.has_value() && nearest->distance <= distance) { return; }
            nearest = RayShapeHit{.distance = distance, .normal = NormalizeOr(outward, -direction)};
        };

        const glm::vec3 span = capsule.end - capsule.start;
        const float lengthSquared = glm::dot(span, span);
        if (lengthSquared > kMinSegmentLengthSquared) {
            // The cylinder, as the circle the ray's path makes across the axis (what moves along the
            // axis does not matter to it). A ray running along the axis can only come in through an
            // end, which is inside the end spheres.
            const float length = std::sqrt(lengthSquared);
            const glm::vec3 axis = span / length;
            const glm::vec3 offset = origin - capsule.start;
            const glm::vec3 offsetAcross = offset - axis * glm::dot(offset, axis);
            const glm::vec3 directionAcross = direction - axis * glm::dot(direction, axis);
            const float speedAcrossSquared = glm::dot(directionAcross, directionAcross);

            if (speedAcrossSquared > kParallelEpsilon) {
                const float speedAcross = std::sqrt(speedAcrossSquared);
                // How far the ray's path across the axis goes to reach the circle, and so how far
                // the ray itself goes.
                if (const std::optional<float> acrossDistance =
                            EnterSphere(offsetAcross, directionAcross / speedAcross, radius)) {
                    const float distance = *acrossDistance / speedAcross;
                    // Only the part of the cylinder between the ends belongs to the capsule.
                    const float along = glm::dot(offset + direction * distance, axis);
                    if (along >= 0.0f && along <= length) { keep(distance, offsetAcross + directionAcross * distance); }
                }
            }
        }

        for (const glm::vec3& center : {capsule.start, capsule.end}) {
            const glm::vec3 offset = origin - center;
            if (const std::optional<float> distance = EnterSphere(offset, direction, radius)) {
                keep(*distance, offset + direction * *distance);
            }
        }

        return nearest;
    }

    std::optional<RayShapeHit> IntersectRay(const Ray& ray, const CollisionShape& shape, const float maxDistance) noexcept {
        return std::visit([&](const auto& value) { return IntersectRay(ray, value, maxDistance); }, shape);
    }

    bool RaycastFilter::Accepts(const EntityId entity, const int layer) const noexcept {
        if ((CollisionLayerBit(layer) & layers) == 0) { return false; }
        return std::find(ignoredEntities.begin(), ignoredEntities.end(), entity) == ignoredEntities.end();
    }

    std::vector<RaycastHit> RaycastAll(const Registry& registry, const Ray& ray, const float maxDistance,
                                       const RaycastFilter& filter) {
        const std::optional<Ray> unit = Detail::NormalizeRay(ray);
        if (!unit.has_value() || !(maxDistance >= 0.0f)) { return {}; }
        return Detail::CollectHits(registry, filter, maxDistance, [&](const CollisionShape& shape, const float reach) {
            return CastRay(*unit, shape, reach);
        });
    }

    std::optional<RaycastHit> RaycastClosest(const Registry& registry, const Ray& ray, const float maxDistance,
                                             const RaycastFilter& filter) {
        const std::optional<Ray> unit = Detail::NormalizeRay(ray);
        if (!unit.has_value() || !(maxDistance >= 0.0f)) { return std::nullopt; }
        return Detail::FindClosestHit(registry, filter, maxDistance, [&](const CollisionShape& shape, const float reach) {
            return CastRay(*unit, shape, reach);
        });
    }

}  // namespace Frost
