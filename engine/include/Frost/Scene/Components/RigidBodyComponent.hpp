#pragma once

#include <glm/vec3.hpp>

namespace Frost {

    // Physical behavior of an object (see PhysicsSystem).
    //
    // A simulated body is moved by the physics simulation: it falls under gravity and, if the
    // object also has a collider (BoxColliderComponent, CapsuleColliderComponent), is stopped by
    // other colliders instead of passing through them. A body that is not simulated stays where it
    // was placed; with a collider it is an immovable obstacle (exactly like a collider without a
    // rigid body).
    //
    // Rotation is not simulated: bodies keep the orientation they were given.
    struct RigidBodyComponent {
        static constexpr const char* kDisplayName = "Rigid Body";

        bool simulated = false;

        // Kilograms. Only matters when two simulated bodies collide: the heavier one is pushed less.
        float mass = 1.0f;

        // --- Simulation state (not saved) -----------------------------------------------------

        // Centimeters per second.
        glm::vec3 linearVelocity{0.0f};

        // Whether it stood on something during the last simulation step: it touched a surface
        // facing mostly up (less than 45 degrees from level). What a character checks before it
        // jumps.
        bool onGround = false;
    };

}  // namespace Frost
