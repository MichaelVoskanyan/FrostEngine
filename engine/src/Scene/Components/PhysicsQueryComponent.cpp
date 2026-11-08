#include <Frost/Scene/Components/PhysicsQueryComponent.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

    constexpr std::array<std::pair<Frost::PhysicsQueryType, std::string_view>, 5> kTypeNames = {{
            {Frost::PhysicsQueryType::Raycast, "raycast"},
            {Frost::PhysicsQueryType::SphereCast, "sphereCast"},
            {Frost::PhysicsQueryType::CapsuleCast, "capsuleCast"},
            {Frost::PhysicsQueryType::SphereOverlap, "sphereOverlap"},
            {Frost::PhysicsQueryType::BoxOverlap, "boxOverlap"},
    }};

    constexpr std::array<std::pair<Frost::PhysicsQueryMode, std::string_view>, 2> kModeNames = {{
            {Frost::PhysicsQueryMode::Closest, "closest"},
            {Frost::PhysicsQueryMode::All, "all"},
    }};

    // Sizes below zero (or NaN) count as zero.
    [[nodiscard]]
    float NonNegative(const float value) noexcept {
        return value >= 0.0f ? value : 0.0f;
    }

}  // namespace

namespace Frost {

    std::string_view GetPhysicsQueryTypeName(const PhysicsQueryType type) noexcept {
        for (const auto& [candidate, name] : kTypeNames) {
            if (candidate == type) { return name; }
        }
        return "raycast";
    }

    std::optional<PhysicsQueryType> ParsePhysicsQueryType(const std::string_view name) noexcept {
        for (const auto& [type, candidate] : kTypeNames) {
            if (candidate == name) { return type; }
        }
        return std::nullopt;
    }

    std::string_view GetPhysicsQueryModeName(const PhysicsQueryMode mode) noexcept {
        for (const auto& [candidate, name] : kModeNames) {
            if (candidate == mode) { return name; }
        }
        return "closest";
    }

    std::optional<PhysicsQueryMode> ParsePhysicsQueryMode(const std::string_view name) noexcept {
        for (const auto& [mode, candidate] : kModeNames) {
            if (candidate == name) { return mode; }
        }
        return std::nullopt;
    }

    bool PhysicsQueryComponent::IsCast() const noexcept {
        return type == PhysicsQueryType::Raycast || type == PhysicsQueryType::SphereCast ||
               type == PhysicsQueryType::CapsuleCast;
    }

    Ray PhysicsQueryComponent::GetWorldRay(const WorldPlacement& at) const noexcept {
        // A direction too short to have one (or broken) falls back to forward.
        const float length = glm::length(direction);
        const glm::vec3 local = length > 1.0e-6f && std::isfinite(length) ? direction / length : glm::vec3(0.0f, 0.0f, -1.0f);
        return Ray{.origin = at.position + at.rotation * offset, .direction = glm::normalize(at.rotation * local)};
    }

    Capsule PhysicsQueryComponent::GetWorldCapsule(const WorldPlacement& at) const noexcept {
        // As a capsule collider: along the object's up axis, no taller than it is wide at least.
        const glm::vec3 center = GetWorldRay(at).origin;
        const float capsuleRadius = NonNegative(radius);
        const float halfSegment = std::max(NonNegative(height) * 0.5f - capsuleRadius, 0.0f);
        const glm::vec3 up = at.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
        return Capsule{.start = center - up * halfSegment, .end = center + up * halfSegment, .radius = capsuleRadius};
    }

    OrientedBox PhysicsQueryComponent::GetWorldBox(const WorldPlacement& at) const noexcept {
        return OrientedBox{.center = GetWorldRay(at).origin,
                           .axes = glm::mat3_cast(at.rotation),
                           .halfExtents = glm::abs(boxSize) * 0.5f};
    }

}  // namespace Frost
