#include "PhysicsQueryLog.hpp"

#include <Frost/Scene/Components/PhysicsQueryComponent.hpp>

#include <iomanip>
#include <sstream>

namespace {

    [[nodiscard]]
    const char* DescribeType(const Frost::PhysicsQueryType type) {
        switch (type) {
            case Frost::PhysicsQueryType::Raycast: return "raycast";
            case Frost::PhysicsQueryType::SphereCast: return "sphere cast";
            case Frost::PhysicsQueryType::CapsuleCast: return "capsule cast";
            case Frost::PhysicsQueryType::SphereOverlap: return "sphere overlap";
            case Frost::PhysicsQueryType::BoxOverlap: return "box overlap";
        }
        return "physics query";
    }

    // "(x, y, z)", to `decimals` decimals.
    [[nodiscard]]
    std::string Format(const glm::vec3& vector, const int decimals) {
        std::ostringstream text;
        text << std::fixed << std::setprecision(decimals) << '(' << vector.x << ", " << vector.y << ", " << vector.z << ')';
        return text.str();
    }

}  // namespace

std::vector<std::string> DescribePhysicsQueryEvents(const Frost::EntityId source, const Frost::PhysicsQueryComponent& query,
                                                    const std::function<std::string(Frost::EntityId)>& nameOf) {
    std::vector<std::string> lines;
    if (!query.debug) { return lines; }

    const std::string prefix = "[Physics Query] '" + nameOf(source) + "' (" + DescribeType(query.type) + ")";
    for (const Frost::EntityId other : query.began) {
        std::ostringstream line;
        line << prefix;
        if (!query.IsCast()) { line << " overlaps '" << nameOf(other) << "'"; }
        else {
            line << " hit '" << nameOf(other) << "'";
            // Casts hit each object once: where.
            for (const Frost::RaycastHit& hit : query.hits) {
                if (hit.entity != other) { continue; }
                line << " at " << std::fixed << std::setprecision(1) << hit.distance << " cm, point "
                     << Format(hit.point, 1) << ", normal " << Format(hit.normal, 2);
                break;
            }
        }
        lines.push_back(line.str());
    }
    for (const Frost::EntityId other : query.ended) {
        lines.push_back(prefix + (query.IsCast() ? " no longer hits '" : " no longer overlaps '") + nameOf(other) + "'");
    }
    return lines;
}
