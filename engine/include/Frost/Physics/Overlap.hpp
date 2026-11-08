#pragma once

#include <Frost/Physics/Collision.hpp>
#include <Frost/Physics/Raycast.hpp>
#include <Frost/Scene/Registry.hpp>

#include <glm/vec3.hpp>

#include <vector>

namespace Frost {

    // Overlap queries: which objects' colliders are (at least partly) inside a volume, e.g. what an
    // explosion reaches, or what stands in a trigger area.
    //
    // They see the same colliders as raycasts, with the same filter (see RaycastFilter and
    // RaycastAll). A collider overlaps the volume when the two share some space, exactly as the
    // PhysicsSystem decides two colliders are in contact (see FindContact): one merely touching the
    // volume's surface does not count. Each object is listed once, by entity id (in a level, its
    // ObjectId; Level::FindObject gives the GameObject), in increasing order.

    // The objects whose colliders overlap the sphere at `center` of `radius`. Nothing for a negative
    // (or non-finite) radius.
    [[nodiscard]]
    std::vector<EntityId> OverlapSphere(const Registry& registry, const glm::vec3& center, float radius,
                                        const RaycastFilter& filter = {});

    // The objects whose colliders overlap `box`, a box in world space at any orientation (e.g.
    // MakeWorldBox of a collider, or OrientedBox{.center, .axes = glm::mat3_cast(rotation),
    // .halfExtents}). Nothing for a box with a negative (or non-finite) half extent.
    [[nodiscard]]
    std::vector<EntityId> OverlapBox(const Registry& registry, const OrientedBox& box, const RaycastFilter& filter = {});

}  // namespace Frost
