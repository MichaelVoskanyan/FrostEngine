#include <Frost/Physics/PhysicsQuerySystem.hpp>

#include <Frost/Physics/Overlap.hpp>
#include <Frost/Physics/Raycast.hpp>
#include <Frost/Physics/ShapeCast.hpp>
#include <Frost/Scene/Components/AttachmentComponent.hpp>
#include <Frost/Scene/Components/PhysicsQueryComponent.hpp>
#include <Frost/Scene/Hierarchy.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace {

    // What the query may hit: its layers, less its own object and what that is attached to (if it
    // ignores itself).
    [[nodiscard]]
    Frost::RaycastFilter MakeFilter(const Frost::Registry& registry, const Frost::EntityId entity,
                                    const Frost::PhysicsQueryComponent& query) {
        Frost::RaycastFilter filter{.layers = query.layers, .ignoredEntities = {}};
        if (!query.ignoreSelf) { return filter; }

        Frost::EntityId current = entity;
        for (std::size_t depth = 0; depth < Frost::kMaxAttachmentDepth && current != 0; ++depth) {
            if (std::find(filter.ignoredEntities.begin(), filter.ignoredEntities.end(), current) !=
                filter.ignoredEntities.end()) {
                break;  // A loop in the chain.
            }
            filter.ignoredEntities.push_back(current);
            const Frost::AttachmentComponent* const attachment = registry.TryGet<Frost::AttachmentComponent>(current);
            current = attachment != nullptr ? attachment->parent : 0;
        }
        return filter;
    }

    // Runs the query itself: its hits (casts), and the objects it touches, in increasing id order.
    void Run(const Frost::Registry& registry, const Frost::PhysicsQueryComponent& query, const Frost::RaycastFilter& filter,
             std::vector<Frost::RaycastHit>& hits, std::vector<Frost::EntityId>& touching) {
        using Frost::PhysicsQueryType;
        const Frost::Ray ray = query.GetWorldRay(query.placement);
        const bool all = query.mode == Frost::PhysicsQueryMode::All;

        // A cast: every hit, or the nearest one.
        const auto keep = [&](auto&& castAll, auto&& castClosest) {
            if (all) { hits = castAll(); }
            else if (std::optional<Frost::RaycastHit> hit = castClosest()) { hits.push_back(*hit); }
        };

        switch (query.type) {
            case PhysicsQueryType::Raycast:
                keep([&] { return Frost::RaycastAll(registry, ray, query.maxDistance, filter); },
                     [&] { return Frost::RaycastClosest(registry, ray, query.maxDistance, filter); });
                break;
            case PhysicsQueryType::SphereCast:
                keep([&] { return Frost::SphereCastAll(registry, ray, query.radius, query.maxDistance, filter); },
                     [&] { return Frost::SphereCastClosest(registry, ray, query.radius, query.maxDistance, filter); });
                break;
            case PhysicsQueryType::CapsuleCast: {
                const Frost::Capsule capsule = query.GetWorldCapsule(query.placement);
                keep([&] { return Frost::CapsuleCastAll(registry, capsule, ray.direction, query.maxDistance, filter); },
                     [&] { return Frost::CapsuleCastClosest(registry, capsule, ray.direction, query.maxDistance, filter); });
                break;
            }
            case PhysicsQueryType::SphereOverlap:
                touching = Frost::OverlapSphere(registry, ray.origin, query.radius, filter);
                return;
            case PhysicsQueryType::BoxOverlap:
                touching = Frost::OverlapBox(registry, query.GetWorldBox(query.placement), filter);
                return;
        }

        for (const Frost::RaycastHit& hit : hits) { touching.push_back(hit.entity); }
        std::sort(touching.begin(), touching.end());
    }

}  // namespace

namespace Frost {

    void UpdatePhysicsQueries(Registry& registry) {
        registry.Each<PhysicsQueryComponent>(
                [&](const EntityId entity, PhysicsQueryComponent& query) { UpdatePhysicsQuery(registry, entity, query); });
    }

    void UpdatePhysicsQuery(const Registry& registry, const EntityId entity, PhysicsQueryComponent& query) {
        std::vector<EntityId> previous = std::move(query.touching);
        query.hits.clear();
        query.touching.clear();
        query.began.clear();
        query.ended.clear();

        if (query.enabled) {
            query.placement = ComputeWorldPlacement(registry, entity);
            query.hasRun = true;
            Run(registry, query, MakeFilter(registry, entity, query), query.hits, query.touching);
        }

        // Both lists are sorted, so the differences are too.
        std::set_difference(query.touching.begin(), query.touching.end(), previous.begin(), previous.end(),
                            std::back_inserter(query.began));
        std::set_difference(previous.begin(), previous.end(), query.touching.begin(), query.touching.end(),
                            std::back_inserter(query.ended));
    }

}  // namespace Frost
