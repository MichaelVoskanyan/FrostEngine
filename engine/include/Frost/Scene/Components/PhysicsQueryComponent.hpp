#pragma once

#include <Frost/Physics/Collision.hpp>
#include <Frost/Physics/CollisionFilter.hpp>
#include <Frost/Physics/Raycast.hpp>
#include <Frost/Scene/Hierarchy.hpp>
#include <Frost/Scene/Ray.hpp>
#include <Frost/Scene/Registry.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Frost {

    // What a PhysicsQueryComponent asks the physics world (see Raycast.hpp, ShapeCast.hpp and
    // Overlap.hpp).
    enum class PhysicsQueryType : std::uint8_t {
        Raycast,        // A ray.
        SphereCast,     // A sphere of `radius`, moved along the direction.
        CapsuleCast,    // A capsule of `radius` and `height`, standing along the object's up axis, moved along the direction.
        SphereOverlap,  // What is inside a sphere of `radius`.
        BoxOverlap,     // What is inside a box of `boxSize`, turned with the object.
    };

    // For casts: the nearest hit only, or every hit along the way.
    enum class PhysicsQueryMode : std::uint8_t {
        Closest,
        All,
    };

    // How each type and mode is called in level files ("raycast", "closest"...), and back (nullopt
    // for an unknown name).
    [[nodiscard]]
    std::string_view GetPhysicsQueryTypeName(PhysicsQueryType type) noexcept;

    [[nodiscard]]
    std::optional<PhysicsQueryType> ParsePhysicsQueryType(std::string_view name) noexcept;

    [[nodiscard]]
    std::string_view GetPhysicsQueryModeName(PhysicsQueryMode mode) noexcept;

    [[nodiscard]]
    std::optional<PhysicsQueryMode> ParsePhysicsQueryMode(std::string_view name) noexcept;

    // A physics query (a raycast, shape cast or overlap) made from the object, every frame, while
    // the game runs (see UpdatePhysicsQueries): what it hits is kept here for gameplay code to read,
    // and drawn with debug drawing.
    //
    // The query is placed by the object's position and rotation in the world (its world placement,
    // so on an attached object it follows the parent, bone or socket), not its scale: the offset,
    // distances and sizes are in centimeters as given. With no offset and the default direction, a
    // raycast starts at the object and goes along its forward (-Z) axis.
    struct PhysicsQueryComponent {
        static constexpr const char* kDisplayName = "Physics Query";

        PhysicsQueryType type = PhysicsQueryType::Raycast;
        PhysicsQueryMode mode = PhysicsQueryMode::Closest;  // Casts only.
        bool enabled = true;

        // Prints what it starts and stops hitting (or overlapping) to the game's log.
        bool debug = false;

        // Where the query starts (casts) or is centered (overlaps), from the object, along its own
        // axes.
        glm::vec3 offset{0.0f};

        // Which way casts go, along the object's own axes (any length; normalized). Default forward.
        glm::vec3 direction{0.0f, 0.0f, -1.0f};

        float maxDistance = 10.0f * kMeter;  // How far casts reach.
        float radius = 25.0f * kCentimeter;  // Sphere and capsule casts, sphere overlaps.
        float height = 1.0f * kMeter;        // Capsule casts: tip to tip, along the object's up axis.
        glm::vec3 boxSize{1.0f * kMeter};    // Box overlaps: full edge lengths, along the object's axes.

        // The collision layers it hits (see RaycastFilter::layers).
        CollisionLayerMask layers = kAllCollisionLayers;

        // Never hits its own object, nor the objects it is attached to (up the chain): a query on a
        // character, or on a weapon the character holds, does not hit the character.
        bool ignoreSelf = true;

        // --- Results of the last update (not saved) ------------------------------------------------

        // Where the query was made from (see above).
        WorldPlacement placement;
        bool hasRun = false;  // Whether it has run at all (and so `placement` and the results are set).

        // Casts: what it hit, nearest first (at most one in Closest mode). Empty for overlaps.
        std::vector<RaycastHit> hits;

        // Every object it hits (casts) or overlaps, in increasing id order; and which of them it did
        // not touch the update before (`began`), and which it touched then but not any more
        // (`ended`).
        std::vector<EntityId> touching;
        std::vector<EntityId> began;
        std::vector<EntityId> ended;

        // --- Geometry, in the world, for a placement ------------------------------------------

        [[nodiscard]]
        bool IsCast() const noexcept;

        // The query's start (or center), and the (unit) direction casts go in.
        [[nodiscard]]
        Ray GetWorldRay(const WorldPlacement& at) const noexcept;

        // The capsule of a capsule cast, centered at the start.
        [[nodiscard]]
        Capsule GetWorldCapsule(const WorldPlacement& at) const noexcept;

        // The box of a box overlap.
        [[nodiscard]]
        OrientedBox GetWorldBox(const WorldPlacement& at) const noexcept;
    };

}  // namespace Frost
