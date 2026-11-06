#include <Frost/Physics/Collision.hpp>

#include <Frost/Units.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

    // Cross products of nearly parallel edges are too short to be a usable direction; those pairs
    // are covered by the boxes' face axes anyway.
    constexpr float kMinAxisLengthSquared = 1.0e-6f;

    // An edge-edge axis must beat the best face axis by this factor to be chosen: face normals
    // give steadier contacts (a box resting on another keeps the same normal frame to frame).
    constexpr float kEdgeAxisPreference = 1.05f;

    // Capsule-box closest points: the distance from the box to a point sliding along the capsule's
    // core segment is a convex function of where on the segment the point is, so a golden-section
    // search finds its minimum. Each step keeps 0.618 of the interval: 48 steps narrow it to ~1e-10
    // of the segment's length.
    constexpr int kClosestPointSearchSteps = 48;

    // Closer than this, a capsule's core segment counts as inside the box it touches.
    constexpr float kInsideDistance = 0.001f * Frost::kCentimeter;

    // Half the box's extent along `axis` (unit length): its "radius" when projected onto it.
    [[nodiscard]]
    float ProjectedRadius(const Frost::OrientedBox& box, const glm::vec3& axis) noexcept {
        return box.halfExtents.x * std::abs(glm::dot(box.axes[0], axis)) +
               box.halfExtents.y * std::abs(glm::dot(box.axes[1], axis)) +
               box.halfExtents.z * std::abs(glm::dot(box.axes[2], axis));
    }

    [[nodiscard]]
    glm::vec3 ClosestPointOnBox(const Frost::OrientedBox& box, const glm::vec3& point) noexcept {
        const glm::vec3 offset = point - box.center;
        glm::vec3 result = box.center;
        for (int i = 0; i < 3; ++i) {
            result += box.axes[i] * glm::clamp(glm::dot(offset, box.axes[i]), -box.halfExtents[i], box.halfExtents[i]);
        }
        return result;
    }

    // The closest points of segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection,
    // 5.1.9), including degenerate (point) segments.
    void ClosestPointsOnSegments(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2,
                                 glm::vec3& onFirst, glm::vec3& onSecond) noexcept {
        // Squared lengths: a segment shorter than 1e-4 cm is a point.
        constexpr float kEpsilon = 1.0e-8f * Frost::kCentimeter * Frost::kCentimeter;
        const glm::vec3 d1 = q1 - p1;
        const glm::vec3 d2 = q2 - p2;
        const glm::vec3 r = p1 - p2;
        const float a = glm::dot(d1, d1);
        const float e = glm::dot(d2, d2);
        const float f = glm::dot(d2, r);

        float s = 0.0f;
        float t = 0.0f;
        if (a <= kEpsilon && e <= kEpsilon) {
            // Both points.
        }
        else if (a <= kEpsilon) {
            t = glm::clamp(f / e, 0.0f, 1.0f);
        }
        else {
            const float c = glm::dot(d1, r);
            if (e <= kEpsilon) { s = glm::clamp(-c / a, 0.0f, 1.0f); }
            else {
                const float b = glm::dot(d1, d2);
                const float denominator = a * e - b * b;  // >= 0; 0 when parallel.
                s = denominator > kEpsilon ? glm::clamp((b * f - c * e) / denominator, 0.0f, 1.0f) : 0.0f;
                t = (b * s + f) / e;
                if (t < 0.0f) {
                    t = 0.0f;
                    s = glm::clamp(-c / a, 0.0f, 1.0f);
                }
                else if (t > 1.0f) {
                    t = 1.0f;
                    s = glm::clamp((b - c) / a, 0.0f, 1.0f);
                }
            }
        }

        onFirst = p1 + d1 * s;
        onSecond = p2 + d2 * t;
    }

}  // namespace

namespace Frost {

    std::array<glm::vec3, 8> OrientedBox::GetCorners() const noexcept {
        std::array<glm::vec3, 8> corners{};
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 sign((i & 1) != 0 ? 1.0f : -1.0f, (i & 2) != 0 ? 1.0f : -1.0f, (i & 4) != 0 ? 1.0f : -1.0f);
            corners[static_cast<std::size_t>(i)] = center + axes * (sign * halfExtents);
        }
        return corners;
    }

    Bounds OrientedBox::GetBounds() const noexcept {
        // The extent along each world axis is the box's projected radius onto it.
        const glm::vec3 radius(ProjectedRadius(*this, glm::vec3(1.0f, 0.0f, 0.0f)),
                               ProjectedRadius(*this, glm::vec3(0.0f, 1.0f, 0.0f)),
                               ProjectedRadius(*this, glm::vec3(0.0f, 0.0f, 1.0f)));
        return Bounds{.min = center - radius, .max = center + radius};
    }

    Bounds Capsule::GetBounds() const noexcept {
        return Bounds{.min = glm::min(start, end) - glm::vec3(radius), .max = glm::max(start, end) + glm::vec3(radius)};
    }

    Capsule MakeWorldCapsule(const TransformComponent& transform, const CapsuleColliderComponent& collider) noexcept {
        const glm::mat3 rotation = glm::mat3_cast(transform.GetRotation());
        const glm::vec3 center = transform.position + rotation * (collider.center * transform.scale);

        // Height with the Y scale, radius with the wider of X and Z: a scaled capsule stays a capsule.
        const glm::vec3 scale = glm::abs(transform.scale);
        const float radius = std::max(collider.radius, 0.0f) * std::max(scale.x, scale.z);
        const float halfSegment = std::max(std::max(collider.height, 0.0f) * scale.y * 0.5f - radius, 0.0f);

        const glm::vec3 up = rotation[1];
        return Capsule{.start = center - up * halfSegment, .end = center + up * halfSegment, .radius = radius};
    }

    Bounds GetBounds(const CollisionShape& shape) noexcept {
        return std::visit([](const auto& value) { return value.GetBounds(); }, shape);
    }

    CollisionShape Inflate(const CollisionShape& shape, const float margin) noexcept {
        if (const OrientedBox* const box = std::get_if<OrientedBox>(&shape)) {
            OrientedBox grown = *box;
            grown.halfExtents += glm::vec3(margin);
            return grown;
        }
        Capsule grown = std::get<Capsule>(shape);
        grown.radius += margin;
        return grown;
    }

    float GetMinHalfExtent(const CollisionShape& shape) noexcept {
        if (const OrientedBox* const box = std::get_if<OrientedBox>(&shape)) {
            return std::min({box->halfExtents.x, box->halfExtents.y, box->halfExtents.z});
        }
        return std::get<Capsule>(shape).radius;
    }

    OrientedBox MakeWorldBox(const TransformComponent& transform, const BoxColliderComponent& collider) noexcept {
        // The same placement as TransformComponent::ToMatrix (translate * rotate * scale), kept as
        // a rotation plus scaled extents: a scaled box is still a box.
        const glm::mat3 rotation = glm::mat3_cast(transform.GetRotation());
        return OrientedBox{.center = transform.position + rotation * (collider.center * transform.scale),
                           .axes = rotation,
                           .halfExtents = glm::abs(collider.size * transform.scale) * 0.5f};
    }

    std::optional<Contact> FindContact(const OrientedBox& a, const OrientedBox& b) noexcept {
        const glm::vec3 offset = b.center - a.center;

        float bestDepth = std::numeric_limits<float>::max();
        glm::vec3 bestNormal{0.0f, 1.0f, 0.0f};

        // False if `axis` separates the boxes; otherwise keeps it if it is the shallowest overlap.
        const auto testAxis = [&](glm::vec3 axis, const bool isEdgeAxis) {
            const float lengthSquared = glm::dot(axis, axis);
            if (lengthSquared < kMinAxisLengthSquared) { return true; }
            axis /= std::sqrt(lengthSquared);

            const float distance = glm::dot(offset, axis);
            const float overlap = ProjectedRadius(a, axis) + ProjectedRadius(b, axis) - std::abs(distance);
            if (!(overlap > 0.0f)) { return false; }  // Apart or just touching (or NaN from a broken transform).

            if (isEdgeAxis ? overlap * kEdgeAxisPreference < bestDepth : overlap < bestDepth) {
                bestDepth = overlap;
                // Away from b: b lies on the +axis side of a when distance > 0.
                bestNormal = distance > 0.0f ? -axis : axis;
            }
            return true;
        };

        // The 15 candidate separating axes: the face normals of each box, and the cross product of
        // every pair of their edge directions.
        for (int i = 0; i < 3; ++i) {
            if (!testAxis(a.axes[i], false)) { return std::nullopt; }
        }
        for (int i = 0; i < 3; ++i) {
            if (!testAxis(b.axes[i], false)) { return std::nullopt; }
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                if (!testAxis(glm::cross(a.axes[i], b.axes[j]), true)) { return std::nullopt; }
            }
        }

        return Contact{.normal = bestNormal, .depth = bestDepth};
    }

    std::optional<Contact> FindContact(const Capsule& a, const OrientedBox& b) noexcept {
        // The closest points of the capsule's core segment and the box (see
        // kClosestPointSearchSteps).
        const glm::vec3 span = a.end - a.start;
        const auto distanceSquaredAt = [&](const float t) {
            const glm::vec3 point = a.start + span * t;
            const glm::vec3 gap = point - ClosestPointOnBox(b, point);
            return glm::dot(gap, gap);
        };

        constexpr float kGolden = 0.6180340f;
        float low = 0.0f;
        float high = 1.0f;
        float left = high - kGolden * (high - low);
        float right = low + kGolden * (high - low);
        float leftValue = distanceSquaredAt(left);
        float rightValue = distanceSquaredAt(right);
        for (int step = 0; step < kClosestPointSearchSteps; ++step) {
            if (leftValue <= rightValue) {
                high = right;
                right = left;
                rightValue = leftValue;
                left = high - kGolden * (high - low);
                leftValue = distanceSquaredAt(left);
            }
            else {
                low = left;
                left = right;
                leftValue = rightValue;
                right = low + kGolden * (high - low);
                rightValue = distanceSquaredAt(right);
            }
        }
        // The search never evaluates the very ends, where the minimum often is (a capsule standing
        // on a box): take whichever of them and the search's result is closest.
        float best = (low + high) * 0.5f;
        for (const float candidate : {0.0f, 1.0f}) {
            if (distanceSquaredAt(candidate) < distanceSquaredAt(best)) { best = candidate; }
        }

        const glm::vec3 onSegment = a.start + span * best;
        const glm::vec3 onBox = ClosestPointOnBox(b, onSegment);
        const glm::vec3 gap = onSegment - onBox;
        const float distance = glm::length(gap);
        if (distance > kInsideDistance) {
            if (distance >= a.radius) { return std::nullopt; }
            return Contact{.normal = gap / distance, .depth = a.radius - distance};
        }

        // The core segment reaches into the box, so there is no gap to go by: push the capsule out
        // through whichever face of the box it is least deep behind (the separating axis test on
        // the box's face normals).
        float bestDepth = std::numeric_limits<float>::max();
        glm::vec3 bestNormal{0.0f, 1.0f, 0.0f};
        for (int i = 0; i < 3; ++i) {
            const glm::vec3& axis = b.axes[i];
            const float boxCenter = glm::dot(b.center, axis);
            const float startAlong = glm::dot(a.start, axis);
            const float endAlong = glm::dot(a.end, axis);
            const float capsuleMin = std::min(startAlong, endAlong) - a.radius;
            const float capsuleMax = std::max(startAlong, endAlong) + a.radius;

            const float outPositive = boxCenter + b.halfExtents[i] - capsuleMin;  // Moving along +axis.
            const float outNegative = capsuleMax - (boxCenter - b.halfExtents[i]);  // Moving along -axis.
            if (outPositive < bestDepth) {
                bestDepth = outPositive;
                bestNormal = axis;
            }
            if (outNegative < bestDepth) {
                bestDepth = outNegative;
                bestNormal = -axis;
            }
        }
        if (!(bestDepth > 0.0f)) { return std::nullopt; }
        return Contact{.normal = bestNormal, .depth = bestDepth};
    }

    std::optional<Contact> FindContact(const OrientedBox& a, const Capsule& b) noexcept {
        std::optional<Contact> contact = FindContact(b, a);
        if (contact.has_value()) { contact->normal = -contact->normal; }
        return contact;
    }

    std::optional<Contact> FindContact(const Capsule& a, const Capsule& b) noexcept {
        glm::vec3 onA;
        glm::vec3 onB;
        ClosestPointsOnSegments(a.start, a.end, b.start, b.end, onA, onB);

        const glm::vec3 gap = onA - onB;
        const float distance = glm::length(gap);
        const float reach = a.radius + b.radius;
        if (!(distance < reach)) { return std::nullopt; }

        // Cores crossing exactly: any direction separates them; up is as good as any.
        const glm::vec3 normal = distance > kInsideDistance ? gap / distance : glm::vec3(0.0f, 1.0f, 0.0f);
        return Contact{.normal = normal, .depth = reach - distance};
    }

    std::optional<Contact> FindContact(const CollisionShape& a, const CollisionShape& b) noexcept {
        return std::visit([](const auto& first, const auto& second) { return FindContact(first, second); }, a, b);
    }

}  // namespace Frost
