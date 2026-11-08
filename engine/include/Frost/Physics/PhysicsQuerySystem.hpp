#pragma once

#include <Frost/Scene/Registry.hpp>

namespace Frost {

    struct PhysicsQueryComponent;

    // Runs every PhysicsQueryComponent of the registry, from where its object is now (see
    // ComputeWorldPlacement), and stores the results in it: its hits, the objects it touches, and
    // which of them it began or stopped touching since the last update. A disabled query touches
    // nothing (so what it touched last ends).
    //
    // Call once per frame, after physics has moved things and poses are up to date (so the query
    // starts where its object is drawn, and sees colliders where they are drawn).
    void UpdatePhysicsQueries(Registry& registry);

    // Runs one query (for `entity`'s component), as UpdatePhysicsQueries does.
    void UpdatePhysicsQuery(const Registry& registry, EntityId entity, PhysicsQueryComponent& query);

}  // namespace Frost
