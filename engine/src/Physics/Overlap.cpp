#include <Frost/Physics/Overlap.hpp>

#include "ColliderQuery.hpp"

#include <algorithm>
#include <cmath>

namespace {

    // The objects whose colliders overlap `volume`, in increasing id order.
    [[nodiscard]]
    std::vector<Frost::EntityId> CollectOverlaps(const Frost::Registry& registry, const Frost::CollisionShape& volume,
                                                 const Frost::RaycastFilter& filter) {
        std::vector<Frost::EntityId> entities;
        Frost::Detail::ForEachCollider(registry, filter, [&](const Frost::EntityId entity, const Frost::CollisionShape& shape) {
            if (Frost::FindContact(volume, shape).has_value()) { entities.push_back(entity); }
        });
        // One collider per object, so each is listed once already.
        std::sort(entities.begin(), entities.end());
        return entities;
    }

}  // namespace

namespace Frost {

    std::vector<EntityId> OverlapSphere(const Registry& registry, const glm::vec3& center, const float radius,
                                        const RaycastFilter& filter) {
        if (!(radius >= 0.0f) || !std::isfinite(radius) || !Detail::IsFinite(center)) { return {}; }
        // A sphere is a capsule whose core is a single point.
        return CollectOverlaps(registry, Capsule{.start = center, .end = center, .radius = radius}, filter);
    }

    std::vector<EntityId> OverlapBox(const Registry& registry, const OrientedBox& box, const RaycastFilter& filter) {
        const glm::vec3& half = box.halfExtents;
        if (!(half.x >= 0.0f && half.y >= 0.0f && half.z >= 0.0f) || !Detail::IsFinite(half) || !Detail::IsFinite(box.center)) {
            return {};
        }
        return CollectOverlaps(registry, box, filter);
    }

}  // namespace Frost
