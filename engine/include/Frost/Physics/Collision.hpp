#pragma once

#include <Frost/Scene/Bounds.hpp>
#include <Frost/Scene/Components/BoxColliderComponent.hpp>
#include <Frost/Scene/Components/CapsuleColliderComponent.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <optional>
#include <variant>

namespace Frost {

    // A box in world space, at any orientation.
    struct OrientedBox {
        glm::vec3 center{0.0f};
        glm::mat3 axes{1.0f};  // Columns: the box's own X, Y and Z directions (unit length).
        glm::vec3 halfExtents{0.5f};

        // The eight corners; corner i is at +/- each half extent by its bits (bit 0: X, 1: Y, 2: Z).
        [[nodiscard]]
        std::array<glm::vec3, 8> GetCorners() const noexcept;

        // The axis-aligned box enclosing it.
        [[nodiscard]]
        Bounds GetBounds() const noexcept;
    };

    // A capsule in world space: every point within `radius` of the segment from `start` to `end`
    // (the centers of its end half-spheres; equal for a sphere).
    struct Capsule {
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        float radius = 0.5f;

        // The axis-aligned box enclosing it.
        [[nodiscard]]
        Bounds GetBounds() const noexcept;
    };

    // Any shape a collider can have.
    using CollisionShape = std::variant<OrientedBox, Capsule>;

    // Where a box collider is in the world, placed by its object's transform.
    [[nodiscard]]
    OrientedBox MakeWorldBox(const TransformComponent& transform, const BoxColliderComponent& collider) noexcept;

    // Where a capsule collider is in the world, placed by its object's transform (see
    // CapsuleColliderComponent for how scale applies).
    [[nodiscard]]
    Capsule MakeWorldCapsule(const TransformComponent& transform, const CapsuleColliderComponent& collider) noexcept;

    // The axis-aligned box enclosing a shape.
    [[nodiscard]]
    Bounds GetBounds(const CollisionShape& shape) noexcept;

    // The shape grown by `margin` in every direction (approximately, for a box: its edges and
    // corners stay sharp).
    [[nodiscard]]
    CollisionShape Inflate(const CollisionShape& shape, float margin) noexcept;

    // Half of the shape's thinnest extent: how far something can sink into it before reaching its
    // middle.
    [[nodiscard]]
    float GetMinHalfExtent(const CollisionShape& shape) noexcept;

    // How two overlapping boxes touch.
    struct Contact {
        // Unit length, pointing from the second box towards the first: moving the first box by
        // normal * depth (or the second by the opposite) separates them.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        float depth = 0.0f;  // How far they overlap along the normal; > 0.
    };

    // The contact between two boxes if they overlap (touching faces do not count), found with the
    // separating axis test: the normal is the direction in which they overlap least.
    [[nodiscard]]
    std::optional<Contact> FindContact(const OrientedBox& a, const OrientedBox& b) noexcept;

    // The contact between a capsule and a box if they overlap: along the line between their closest
    // points (or, if the capsule's core segment reaches into the box, out of the box face it is
    // least deep behind).
    [[nodiscard]]
    std::optional<Contact> FindContact(const Capsule& a, const OrientedBox& b) noexcept;

    [[nodiscard]]
    std::optional<Contact> FindContact(const OrientedBox& a, const Capsule& b) noexcept;

    // The contact between two capsules if they overlap: along the line between the closest points
    // of their core segments.
    [[nodiscard]]
    std::optional<Contact> FindContact(const Capsule& a, const Capsule& b) noexcept;

    // Any two shapes (see the overloads above). The normal points from `b` towards `a`.
    [[nodiscard]]
    std::optional<Contact> FindContact(const CollisionShape& a, const CollisionShape& b) noexcept;

}  // namespace Frost
