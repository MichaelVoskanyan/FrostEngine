#include <Frost/Physics/ShapeCast.hpp>

#include "ColliderQuery.hpp"

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <utility>

// How a shape cast is worked out. The moving shape is a capsule: the points within `radius` of a
// core segment (a sphere's core is a single point). It first touches a target where their closest
// features first come within reach of each other, so every pair of features that can be the
// closest is tried, and the earliest contact of any pair is the shape's first contact:
//
//   1. an end of the core (an end sphere) against the whole target: a ray from the end against
//      the target grown by the radius (for a box: the box widened across each of its faces, and
//      capsules around its edges, which together make the rounded box);
//   2. the middle of the core against the middle of the target's edges (for a box: its 12 edges;
//      for a capsule: its core): the moment the two lines come within reach, if that is between
//      the ends of both;
//   3. the middle of the core against the target's corners (for a box: its 8 corners; for a
//      capsule: the ends of its core): a ray from the corner, backwards, against the moving
//      capsule.
//
// That is every case: the middle of the core only touches a box's face first if it lies parallel to
// the face, and then it touches all along the part of it over the face, whose ends are ends of the
// core (1), places where it crosses the face's edges (2) or, if it runs along an edge, the face's
// corners (3). Each test is exact (no stepping), so a cast is as precise as a raycast. For a
// sphere, 1 alone is the whole answer.

namespace {

    // Core segments shorter than this (squared) are points: the capsule is a sphere.
    constexpr float kMinSegmentLengthSquared = 1.0e-8f * Frost::kCentimeter * Frost::kCentimeter;

    // Lines closer to parallel than this (the squared sine of the angle between them) cannot touch
    // middle to middle before their ends do (see 2 above).
    constexpr float kParallelSineSquared = 1.0e-10f;

    // A contact only counts if the shape is moving into the surface, at least this much (the sine
    // of the angle between its direction and the surface): a shape sliding along a surface it
    // touches, or passing an edge with no room to spare, is not stopped by it, so it is not a hit.
    // (At this angle, a 40 cm sphere would sink in 2e-7 cm at most before moving out again.)
    constexpr float kMinApproach = 1.0e-4f;

    // The 12 edges of a box, as pairs of corner indices (see OrientedBox::GetCorners) that differ in
    // exactly one axis bit.
    constexpr std::array<std::pair<int, int>, 12> kBoxEdges = {{{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                                               {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                                               {0, 4}, {1, 5}, {2, 6}, {3, 7}}};

    // The earliest contact found so far, among those within reach that the shape moves into.
    class EarliestContact {
    public:
        EarliestContact(const glm::vec3& direction, const float maxDistance) noexcept
            : m_Direction(direction), m_MaxDistance(maxDistance) {}

        void Offer(const float distance, const glm::vec3& point, const glm::vec3& normal) noexcept {
            if (!(distance >= 0.0f) || distance > m_MaxDistance) { return; }
            if (!(glm::dot(normal, m_Direction) < -kMinApproach)) { return; }  // Grazing (see kMinApproach).
            if (m_Best.has_value() && m_Best->distance <= distance) { return; }
            m_Best = Frost::ShapeCastHit{.distance = distance, .point = point, .normal = normal};
        }

        // Offers ray hits only up to here (nothing later can win).
        [[nodiscard]]
        float GetReach() const noexcept {
            return m_Best.has_value() ? m_Best->distance : m_MaxDistance;
        }

        [[nodiscard]]
        const std::optional<Frost::ShapeCastHit>& Get() const noexcept {
            return m_Best;
        }

    private:
        glm::vec3 m_Direction;
        float m_MaxDistance;
        std::optional<Frost::ShapeCastHit> m_Best;
    };

    // The moving capsule, with its direction normalized.
    struct Sweep {
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        float radius = 0.0f;
        glm::vec3 direction{0.0f};

        [[nodiscard]]
        bool IsSphere() const noexcept {
            const glm::vec3 span = end - start;
            return glm::dot(span, span) <= kMinSegmentLengthSquared;
        }
    };

    // The sweep, if the cast is well formed (a usable direction, finite ends, a radius >= 0).
    [[nodiscard]]
    std::optional<Sweep> MakeSweep(const Frost::Capsule& capsule, const glm::vec3& direction, const float maxDistance) noexcept {
        const std::optional<glm::vec3> unit = Frost::Detail::NormalizeDirection(direction);
        if (!unit.has_value() || !(maxDistance >= 0.0f) || !(capsule.radius >= 0.0f) || !std::isfinite(capsule.radius) ||
            !Frost::Detail::IsFinite(capsule.start) || !Frost::Detail::IsFinite(capsule.end)) {
            return std::nullopt;
        }
        return Sweep{.start = capsule.start, .end = capsule.end, .radius = capsule.radius, .direction = *unit};
    }

    // 1: the sphere of the sweep's radius at `center`, moving, against a target grown by that
    // radius. `grownHit` is a ray hit on the grown target; the contact is `radius` back from the
    // sphere's center at that moment, against the normal.
    void OfferEndSphere(const Sweep& sweep, const glm::vec3& center, const std::optional<Frost::RayShapeHit>& grownHit,
                        EarliestContact& earliest) noexcept {
        if (!grownHit.has_value()) { return; }
        const glm::vec3 centerThen = center + sweep.direction * grownHit->distance;
        earliest.Offer(grownHit->distance, centerThen - grownHit->normal * sweep.radius, grownHit->normal);
    }

    void EndSphereAgainstBox(const Sweep& sweep, const glm::vec3& center, const Frost::OrientedBox& box,
                             const std::array<glm::vec3, 8>& corners, EarliestContact& earliest) noexcept {
        const Frost::Ray ray{.origin = center, .direction = sweep.direction};

        // The rounded box is the box widened across each pair of its faces, together with capsules
        // around its edges (their end spheres round its corners).
        for (int i = 0; i < 3; ++i) {
            Frost::OrientedBox widened = box;
            widened.halfExtents[i] += sweep.radius;
            OfferEndSphere(sweep, center, Frost::IntersectRay(ray, widened, earliest.GetReach()), earliest);
        }
        if (sweep.radius > 0.0f) {
            for (const auto& [first, second] : kBoxEdges) {
                const Frost::Capsule edge{.start = corners[static_cast<std::size_t>(first)],
                                          .end = corners[static_cast<std::size_t>(second)],
                                          .radius = sweep.radius};
                OfferEndSphere(sweep, center, Frost::IntersectRay(ray, edge, earliest.GetReach()), earliest);
            }
        }
    }

    // 2: the middle of the sweep's core against the middle of the segment `from`-`to` (grown by
    // `targetRadius`), if they come within reach of each other between the ends of both.
    void MiddleAgainstSegment(const Sweep& sweep, const glm::vec3& from, const glm::vec3& to, const float targetRadius,
                              EarliestContact& earliest) noexcept {
        const glm::vec3 core = sweep.end - sweep.start;
        const glm::vec3 segment = to - from;
        const glm::vec3 across = glm::cross(core, segment);
        const float acrossSquared = glm::dot(across, across);
        const float coreSquared = glm::dot(core, core);
        const float segmentSquared = glm::dot(segment, segment);
        if (!(acrossSquared > kParallelSineSquared * coreSquared * segmentSquared)) { return; }  // Parallel, or a point.

        // Middle to middle, the closest points of the two lines are along their common
        // perpendicular, which translating the core does not turn: they touch when the core's line
        // has come to within reach of the segment's along it, from whichever side it is on.
        const glm::vec3 perpendicular = across / std::sqrt(acrossSquared);
        const float reach = sweep.radius + targetRadius;
        for (const float side : {1.0f, -1.0f}) {
            const glm::vec3 normal = perpendicular * side;  // From the segment towards the core.
            const float approach = glm::dot(sweep.direction, normal);
            if (!(approach < 0.0f)) { continue; }  // Moving away from this side (or along it).

            const float distance = (reach - glm::dot(sweep.start - from, normal)) / approach;
            if (!(distance >= 0.0f) || distance > earliest.GetReach()) { continue; }

            // Where on each segment the lines' closest points are at that moment (Ericson,
            // Real-Time Collision Detection, 5.1.8); both must be between the ends.
            const glm::vec3 offset = sweep.start + sweep.direction * distance - from;
            const float coreDotSegment = glm::dot(core, segment);
            const float denominator = coreSquared * segmentSquared - coreDotSegment * coreDotSegment;
            const float coreDotOffset = glm::dot(core, offset);
            const float segmentDotOffset = glm::dot(segment, offset);
            const float onCore = (coreDotSegment * segmentDotOffset - segmentSquared * coreDotOffset) / denominator;
            const float onSegment = (coreSquared * segmentDotOffset - coreDotSegment * coreDotOffset) / denominator;
            if (!(onCore >= 0.0f && onCore <= 1.0f && onSegment >= 0.0f && onSegment <= 1.0f)) { continue; }

            earliest.Offer(distance, from + segment * onSegment + normal * targetRadius, normal);
        }
    }

    // 3: a corner of the target (a sphere of `targetRadius` at `corner`) against the sweep: a ray from
    // the corner, backwards along the direction, against the sweep's capsule grown by the corner's
    // radius.
    void CornerAgainstSweep(const Sweep& sweep, const glm::vec3& corner, const float targetRadius,
                            EarliestContact& earliest) noexcept {
        const Frost::Ray ray{.origin = corner, .direction = -sweep.direction};
        const Frost::Capsule grown{.start = sweep.start, .end = sweep.end, .radius = sweep.radius + targetRadius};
        const std::optional<Frost::RayShapeHit> hit = Frost::IntersectRay(ray, grown, earliest.GetReach());
        if (!hit.has_value()) { return; }

        // The ray's normal points out of the sweep; the target's, at the contact, the other way.
        const glm::vec3 normal = -hit->normal;
        earliest.Offer(hit->distance, corner + normal * targetRadius, normal);
    }

    // A cast's hit on one collider, for the registry-level casts (the entity is filled in by the
    // caller).
    [[nodiscard]]
    std::optional<Frost::RaycastHit> CastOne(const Sweep& sweep, const Frost::CollisionShape& shape,
                                             const float maxDistance) noexcept {
        const Frost::Capsule capsule{.start = sweep.start, .end = sweep.end, .radius = sweep.radius};
        const std::optional<Frost::ShapeCastHit> hit = Frost::CastCapsule(capsule, sweep.direction, maxDistance, shape);
        if (!hit.has_value()) { return std::nullopt; }
        return Frost::RaycastHit{.entity = 0, .distance = hit->distance, .point = hit->point, .normal = hit->normal};
    }

}  // namespace

namespace Frost {

    std::optional<ShapeCastHit> CastCapsule(const Capsule& capsule, const glm::vec3& direction, const float maxDistance,
                                            const OrientedBox& target) noexcept {
        const std::optional<Sweep> sweep = MakeSweep(capsule, direction, maxDistance);
        if (!sweep.has_value() || FindContact(capsule, target).has_value()) { return std::nullopt; }

        EarliestContact earliest(sweep->direction, maxDistance);
        const std::array<glm::vec3, 8> corners = target.GetCorners();

        EndSphereAgainstBox(*sweep, sweep->start, target, corners, earliest);
        if (!sweep->IsSphere()) {
            EndSphereAgainstBox(*sweep, sweep->end, target, corners, earliest);
            for (const auto& [first, second] : kBoxEdges) {
                MiddleAgainstSegment(*sweep, corners[static_cast<std::size_t>(first)],
                                     corners[static_cast<std::size_t>(second)], 0.0f, earliest);
            }
            for (const glm::vec3& corner : corners) { CornerAgainstSweep(*sweep, corner, 0.0f, earliest); }
        }
        return earliest.Get();
    }

    std::optional<ShapeCastHit> CastCapsule(const Capsule& capsule, const glm::vec3& direction, const float maxDistance,
                                            const Capsule& target) noexcept {
        const std::optional<Sweep> sweep = MakeSweep(capsule, direction, maxDistance);
        if (!sweep.has_value() || !(target.radius >= 0.0f) || FindContact(capsule, target).has_value()) {
            return std::nullopt;
        }

        EarliestContact earliest(sweep->direction, maxDistance);
        const Capsule grown{.start = target.start, .end = target.end, .radius = target.radius + sweep->radius};
        const auto endSphere = [&](const glm::vec3& center) {
            const Ray ray{.origin = center, .direction = sweep->direction};
            OfferEndSphere(*sweep, center, IntersectRay(ray, grown, earliest.GetReach()), earliest);
        };

        endSphere(sweep->start);
        if (!sweep->IsSphere()) {
            endSphere(sweep->end);
            MiddleAgainstSegment(*sweep, target.start, target.end, target.radius, earliest);
            CornerAgainstSweep(*sweep, target.start, target.radius, earliest);
            CornerAgainstSweep(*sweep, target.end, target.radius, earliest);
        }
        return earliest.Get();
    }

    std::optional<ShapeCastHit> CastCapsule(const Capsule& capsule, const glm::vec3& direction, const float maxDistance,
                                            const CollisionShape& target) noexcept {
        return std::visit([&](const auto& value) { return CastCapsule(capsule, direction, maxDistance, value); }, target);
    }

    std::vector<RaycastHit> SphereCastAll(const Registry& registry, const Ray& ray, const float radius,
                                          const float maxDistance, const RaycastFilter& filter) {
        return CapsuleCastAll(registry, Capsule{.start = ray.origin, .end = ray.origin, .radius = radius}, ray.direction,
                              maxDistance, filter);
    }

    std::optional<RaycastHit> SphereCastClosest(const Registry& registry, const Ray& ray, const float radius,
                                                const float maxDistance, const RaycastFilter& filter) {
        return CapsuleCastClosest(registry, Capsule{.start = ray.origin, .end = ray.origin, .radius = radius},
                                  ray.direction, maxDistance, filter);
    }

    std::vector<RaycastHit> CapsuleCastAll(const Registry& registry, const Capsule& capsule, const glm::vec3& direction,
                                           const float maxDistance, const RaycastFilter& filter) {
        const std::optional<Sweep> sweep = MakeSweep(capsule, direction, maxDistance);
        if (!sweep.has_value()) { return {}; }
        return Detail::CollectHits(registry, filter, maxDistance, [&](const CollisionShape& shape, const float reach) {
            return CastOne(*sweep, shape, reach);
        });
    }

    std::optional<RaycastHit> CapsuleCastClosest(const Registry& registry, const Capsule& capsule,
                                                 const glm::vec3& direction, const float maxDistance,
                                                 const RaycastFilter& filter) {
        const std::optional<Sweep> sweep = MakeSweep(capsule, direction, maxDistance);
        if (!sweep.has_value()) { return std::nullopt; }
        return Detail::FindClosestHit(registry, filter, maxDistance, [&](const CollisionShape& shape, const float reach) {
            return CastOne(*sweep, shape, reach);
        });
    }

}  // namespace Frost
