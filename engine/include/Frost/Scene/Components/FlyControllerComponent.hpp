#pragma once

#include <Frost/Scene/Components/TransformComponent.hpp>
#include <Frost/Units.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace Frost {

    class Input;

    // One frame of the player's intent for a FlyControllerComponent, as read from the keyboard and
    // mouse by FlyControllerComponent::PollInput. Kept separate from the polling so something else
    // (input priority, a replay, a test) can supply it instead.
    struct FlyInput {
        // Wanted movement: x = right, y = up (world), z = forward; each -1..1.
        glm::vec3 move{0.0f};
        // Mouse movement to look with, in screen units (+x right, +y down); zero when the mouse is
        // not taken for looking.
        glm::vec2 look{0.0f};
        // Mouse wheel notches (+ = away from the user), to change the speed with.
        float speedSteps = 0.0f;
        bool fast = false;
    };

    // Flies its object around from the player's input, with no gravity and no collision: the
    // game's free camera (put it on an object with a CameraComponent), and a stand-in for a player
    // character until there is one.
    //
    // It drives the object's TransformComponent directly: the mouse turns it (yaw about world up,
    // and pitch, stopping short of straight up/down; roll is left alone), W/A/S/D move it along
    // where it faces and sideways, Space / Left Ctrl straight up and down, Left Shift faster, and
    // the mouse wheel changes the speed. Mouse movement only counts while the mouse is captured
    // (taken for looking around); the game decides when that is.
    struct FlyControllerComponent {
        static constexpr const char* kDisplayName = "Fly Controller";

        float moveSpeed = 3.0f * kMeter;  // Centimeters per second (the wheel changes it while playing).
        float fastMultiplier = 4.0f;      // Speed factor while Left Shift is held.
        float lookSensitivity = 0.115f;   // Degrees of turn per screen unit of mouse movement.

        // This frame's input, from the keyboard and mouse.
        [[nodiscard]]
        static FlyInput PollInput(const Input& input);

        // Moves and turns `transform` by `input` over `deltaTime` seconds (long frames are capped,
        // so a hitch does not throw the object across the level).
        void Apply(const FlyInput& input, float deltaTime, TransformComponent& transform);

        // PollInput, then Apply: what the game calls every frame for each controlled object.
        void Update(const Input& input, float deltaTime, TransformComponent& transform);
    };

}  // namespace Frost
