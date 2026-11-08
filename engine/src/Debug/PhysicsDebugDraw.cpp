#include <Frost/Debug/PhysicsDebugDraw.hpp>

#include <Frost/Scene/Components/PhysicsQueryComponent.hpp>

#include "../Physics/ColliderQuery.hpp"

#include <glm/geometric.hpp>

#include <cstdio>
#include <optional>
#include <variant>

namespace {

    constexpr Frost::DebugColor kClearColor = Frost::DebugColors::kGreen;     // Found nothing.
    constexpr Frost::DebugColor kHitColor = Frost::DebugColors::kRed;         // Found something.
    constexpr Frost::DebugColor kUnusedColor = Frost::DebugColors::kGray;     // Not looked at.
    constexpr Frost::DebugColor kTouchedColor = Frost::DebugColors::kOrange;  // What it touches.
    constexpr Frost::DebugColor kPointColor = Frost::DebugColors::kYellow;
    constexpr Frost::DebugColor kNormalColor = Frost::DebugColors::kCyan;

    constexpr float kPointSize = 10.0f * Frost::kCentimeter;
    constexpr float kNormalLength = 30.0f * Frost::kCentimeter;

    [[nodiscard]]
    std::string DistanceText(const float centimeters) {
        char buffer[32];
        if (centimeters >= 100.0f * Frost::kCentimeter) { std::snprintf(buffer, sizeof(buffer), "%.2f m", centimeters / Frost::kMeter); }
        else { std::snprintf(buffer, sizeof(buffer), "%.1f cm", centimeters); }
        return buffer;
    }

    [[nodiscard]]
    std::string NameOf(const Frost::DebugNameLookup& nameOf, const Frost::EntityId entity) {
        std::string name = nameOf ? nameOf(entity) : std::string();
        return name.empty() ? "#" + std::to_string(entity) : name;
    }

    // The cast's shape with its start at `center` (a point for a raycast).
    void DrawCastShape(Frost::DebugDraw& draw, const Frost::PhysicsQueryComponent& query, const glm::vec3& center,
                       const Frost::DebugColor& color) {
        const Frost::Ray ray = query.GetWorldRay(query.placement);
        switch (query.type) {
            case Frost::PhysicsQueryType::SphereCast: draw.DrawSphere(center, query.radius, color); break;
            case Frost::PhysicsQueryType::CapsuleCast: {
                Frost::Capsule capsule = query.GetWorldCapsule(query.placement);
                capsule.start += center - ray.origin;
                capsule.end += center - ray.origin;
                draw.DrawCapsule(capsule, color);
                break;
            }
            default: draw.DrawPoint(center, kPointSize, color); break;
        }
    }

    void DrawCast(Frost::DebugDraw& draw, const Frost::PhysicsQueryComponent& query, const Frost::DebugNameLookup& nameOf) {
        const Frost::Ray ray = query.GetWorldRay(query.placement);
        const float reach = query.maxDistance >= 0.0f ? query.maxDistance : 0.0f;
        const glm::vec3 end = ray.origin + ray.direction * reach;

        DrawCastShape(draw, query, ray.origin, query.hits.empty() ? kClearColor : kHitColor);

        if (query.hits.empty()) {
            draw.DrawArrow(ray.origin, end, kClearColor, 2.0f);
            DrawCastShape(draw, query, end, kUnusedColor);  // As far as it reached.
            return;
        }

        // Red as far as the last hit; beyond it, green if it went on looking (All), else gray.
        const float lastDistance = query.hits.back().distance;
        const glm::vec3 last = ray.origin + ray.direction * lastDistance;
        draw.DrawLine(ray.origin, last, kHitColor, 2.0f);
        draw.DrawArrow(last, end, query.mode == Frost::PhysicsQueryMode::All ? kClearColor : kUnusedColor, 1.0f);

        for (const Frost::RaycastHit& hit : query.hits) {
            // The shape where it touched, the point, the surface's normal, and how far it went.
            DrawCastShape(draw, query, ray.origin + ray.direction * hit.distance, kHitColor);
            draw.DrawPoint(hit.point, kPointSize, kPointColor);
            draw.DrawArrow(hit.point, hit.point + hit.normal * kNormalLength, kNormalColor);
            draw.DrawLabel(hit.point, NameOf(nameOf, hit.entity) + "  " + DistanceText(hit.distance), kPointColor);
        }
    }

}  // namespace

namespace Frost {

    void DrawCollider(DebugDraw& draw, const Registry& registry, const EntityId entity, const DebugColor& color) {
        if (const std::optional<Detail::WorldCollider> collider = Detail::FindWorldCollider(registry, entity)) {
            draw.DrawShape(collider->shape, color);
        }
    }

    void DrawPhysicsQuery(DebugDraw& draw, const Registry& registry, const PhysicsQueryComponent& query,
                          const DebugNameLookup& nameOf) {
        if (!query.enabled || !query.hasRun) { return; }

        if (query.IsCast()) { DrawCast(draw, query, nameOf); }
        else {
            const DebugColor color = query.touching.empty() ? kClearColor : kHitColor;
            if (query.type == PhysicsQueryType::SphereOverlap) {
                draw.DrawSphere(query.GetWorldRay(query.placement).origin, query.radius, color);
            }
            else { draw.DrawBox(query.GetWorldBox(query.placement), color); }
        }

        // What it touches: their colliders, and (for overlaps, which have no hit points) their names.
        for (const EntityId entity : query.touching) {
            const std::optional<Detail::WorldCollider> collider = Detail::FindWorldCollider(registry, entity);
            if (!collider.has_value()) { continue; }
            draw.DrawShape(collider->shape, kTouchedColor);
            if (!query.IsCast()) { draw.DrawLabel(GetBounds(collider->shape).GetCenter(), NameOf(nameOf, entity), kTouchedColor); }
        }
    }

}  // namespace Frost
