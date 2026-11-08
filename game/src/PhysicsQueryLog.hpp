#pragma once

#include <Frost/Scene/Registry.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Frost {
    struct PhysicsQueryComponent;
}

// What a physics query with `debug` set reports about its last update (see
// Frost::UpdatePhysicsQueries): one line for each object it began touching (for a cast, where it
// hit it: distance, point and normal) and one for each it stopped touching. Nothing if neither
// happened, or if it does not have `debug` set.
//
// `nameOf` names objects (the query's own, `source`, and those it touches).
[[nodiscard]]
std::vector<std::string> DescribePhysicsQueryEvents(Frost::EntityId source, const Frost::PhysicsQueryComponent& query,
                                                    const std::function<std::string(Frost::EntityId)>& nameOf);
