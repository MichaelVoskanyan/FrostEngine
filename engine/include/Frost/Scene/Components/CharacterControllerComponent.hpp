#pragma once

#include <Frost/Scene/Components/CameraComponent.hpp>
#include <Frost/Scene/Components/RigidBodyComponent.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>
#include <Frost/Units.hpp>

#include <glm/vec2.hpp>

namespace Frost {

    class Input;

    // One frame of the player's intent for a CharacterControllerComponent, as read from the keyboard
    // and mouse by CharacterControllerComponent::PollInput.
    struct CharacterInput {
        // Wanted walking direction relative to where the character faces: x = right, y = forward;
        // each -1..1.
        glm::vec2 move{0.0f};
        // Mouse movement to look with, in screen units (+x right, +y down); zero when the mouse is
        // not taken for looking.
        glm::vec2 look{0.0f};
        // Jump was pressed this frame.
        bool jump = false;
    };

    // Walks its object around like a first-person character, from the player's input. The object
    // is the character's body: a simulated RigidBodyComponent (so gravity and collisions apply)
    // with a collider, usually a CapsuleColliderComponent, and a CameraComponent mounted at head
    // height to see through.
    //
    //   - Mouse left/right turns the body (the yaw of its TransformComponent) and so everything
    //     mounted on it; mouse up/down only tilts the camera (the pitch of its CameraComponent's
    //     rotation), stopping short of straight up/down. The body stays upright.
    //   - W/A/S/D walk on the ground plane (x, z), relative to where the body faces, by setting the
    //     body's horizontal velocity; physics does the rest (falling, being stopped by walls).
    //   - Space jumps, if the body is standing on something (RigidBodyComponent::onGround).
    //
    // Mouse movement only counts while the mouse is captured; the game decides when that is.
    struct CharacterControllerComponent {
        static constexpr const char* kDisplayName = "Character Controller";

        float walkSpeed = 5.0f * kMeter;  // Centimeters per second.
        float jumpSpeed = 5.0f * kMeter;  // Upward speed at take-off, cm/s (5 m/s lifts about 1.27 m).
        float lookSensitivity = 0.115f;   // Degrees of turn per screen unit of mouse movement.

        // This frame's input, from the keyboard and mouse.
        [[nodiscard]]
        static CharacterInput PollInput(const Input& input);

        // Turns the body (`transform`) and the camera (`camera`, if the object has one: only its
        // pitch), and sets the body's walking velocity and jump (`body`).
        void Apply(const CharacterInput& input, TransformComponent& transform, RigidBodyComponent& body,
                   CameraComponent* camera) const;

        // PollInput, then Apply: what the game calls every frame for each character.
        void Update(const Input& input, TransformComponent& transform, RigidBodyComponent& body,
                    CameraComponent* camera) const;
    };

}  // namespace Frost
