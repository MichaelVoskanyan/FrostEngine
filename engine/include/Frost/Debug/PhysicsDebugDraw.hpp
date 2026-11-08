#pragma once

#include <Frost/Debug/DebugDraw.hpp>
#include <Frost/Scene/Registry.hpp>

#include <functional>
#include <string>

namespace Frost {

    struct PhysicsQueryComponent;

    // How debug drawing names an object in its labels (e.g. from the level's GameObjects); empty
    // for its id.
    using DebugNameLookup = std::function<std::string(EntityId)>;

    // Draws a physics query as it last ran (see UpdatePhysicsQueries); nothing if it is disabled or
    // has not run:
    //   - casts: the shape at its start, the way it goes up to its reach, the shape where it touched
    //     each hit, and each hit's point, normal and distance. The way is green where it found
    //     nothing, red up to its last hit, and gray past a Closest query's hit (not looked at);
    //   - overlaps: the volume, green while empty and red while it contains something;
    //   - for both, the colliders of the objects it touches, in orange, with their names.
    void DrawPhysicsQuery(DebugDraw& draw, const Registry& registry, const PhysicsQueryComponent& query,
                          const DebugNameLookup& nameOf = {});

    // The collider of an object (as the PhysicsSystem sees it), if it has one.
    void DrawCollider(DebugDraw& draw, const Registry& registry, EntityId entity, const DebugColor& color);

}  // namespace Frost
