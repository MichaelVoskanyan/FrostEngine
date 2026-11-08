#pragma once

#include <Frost/Physics/Collision.hpp>
#include <Frost/Physics/CollisionFilter.hpp>
#include <Frost/Scene/Registry.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>

#include <vector>

namespace Frost {

    struct BoxColliderComponent;
    struct CapsuleColliderComponent;
    struct RigidBodyComponent;

    struct PhysicsSettings {
        // Centimeters per second squared; +Y is up.
        glm::vec3 gravity{0.0f, -kStandardGravity, 0.0f};

        // The simulation always advances in steps of this many seconds, however long frames are,
        // so it behaves the same at any frame rate.
        float fixedTimeStep = 1.0f / 120.0f;

        // How many times per step overlapping pairs are pushed apart. More settles stacks of
        // simulated bodies better.
        int solverIterations = 4;

        // How quickly sliding contacts slow down (Coulomb friction coefficient; 0 = ice).
        float friction = 0.5f;
    };

    // A basic rigid body simulation, run on a Registry's components:
    //   - Objects with a simulated RigidBodyComponent fall under gravity.
    //   - If such an object also has a collider (a BoxColliderComponent or a
    //     CapsuleColliderComponent; an object with both uses its box), it collides: it is pushed out
    //     of the colliders of other objects and stops moving into them (no bounce, some friction).
    //     Colliders of objects without a simulated rigid body (e.g. the ground) never move.
    //     Two simulated bodies push each other apart according to their masses.
    //   - Two colliders only collide if their collision layers and masks say so (see
    //     CollisionFilter::CollidesWith); otherwise they pass through each other.
    //   - Whether each body is standing on something is recorded (RigidBodyComponent::onGround).
    //   - Rotation is not simulated; bodies keep their orientation.
    //   - Objects attached to other objects (AttachmentComponent) move with them and are left out
    //     entirely: neither simulated nor collided with.
    //
    // Positions are written straight into the objects' TransformComponents.
    class PhysicsSystem final {
    public:
        explicit PhysicsSystem(const PhysicsSettings& settings = {});

        // Advances the simulation by `deltaTime` seconds (one frame), in fixed steps; time left over
        // is carried to the next call. Long frames (a hitch, a breakpoint) are capped, so the
        // simulation slows down rather than jumping ahead.
        void Update(Registry& registry, float deltaTime);

        // Advances the simulation by exactly one step of `timeStep` seconds.
        void Step(Registry& registry, float timeStep);

        // Forgets any carried-over time (e.g. when the simulation is started again).
        void Reset() noexcept;

        [[nodiscard]]
        const PhysicsSettings& GetSettings() const noexcept;

        void SetSettings(const PhysicsSettings& settings) noexcept;

    private:
        // A simulated body taking part in the current step (pointers into the registry, valid for
        // the duration of the step only).
        struct Body {
            TransformComponent* transform = nullptr;
            RigidBodyComponent* rigidBody = nullptr;
            // Its collider (at most one is set; neither: it falls, but collides with nothing).
            const BoxColliderComponent* box = nullptr;
            const CapsuleColliderComponent* capsule = nullptr;
            float inverseMass = 1.0f;

            // Its collider's layer and mask (only if Collides()).
            [[nodiscard]]
            const CollisionFilter& GetFilter() const noexcept;

            [[nodiscard]]
            bool Collides() const noexcept;

            // Where its collider is now (only if Collides()).
            [[nodiscard]]
            CollisionShape GetShape() const noexcept;
        };

        void Gather(Registry& registry);
        [[nodiscard]]
        int CountSubSteps(float timeStep) const;

        // Before moving: stops bodies moving into what they already touch (see the .cpp).
        void StopAtTouchingContacts(float timeStep);
        // After moving: pushes overlapping bodies apart and stops them moving into each other.
        void ResolveOverlaps();

        // Velocity changes for one contact: the body (or the pair) may approach along -normal by at
        // most `allowedApproachSpeed`; anything faster is removed, with friction.
        void StopBody(Body& body, const glm::vec3& normal, float allowedApproachSpeed) const;
        void StopPair(Body& first, Body& second, const glm::vec3& normal, float allowedApproachSpeed) const;

        PhysicsSettings m_Settings;
        float m_Accumulator = 0.0f;

        // A collider that does not move, where it is this step.
        struct Obstacle {
            CollisionShape shape;
            CollisionFilter filter;
        };

        // Rebuilt every step; kept to reuse their storage.
        std::vector<Body> m_Bodies;
        std::vector<Obstacle> m_Obstacles;
    };

}  // namespace Frost
