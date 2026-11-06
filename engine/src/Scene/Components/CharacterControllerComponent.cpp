#include <Frost/Scene/Components/CharacterControllerComponent.hpp>

#include <Frost/Platform/Input.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace {

    // Just short of straight up/down: at exactly +-90 degrees the view's "up" stops being meaningful.
    constexpr float kMaxPitch = 89.0f;

    // -1, 0 or +1 from a pair of opposing keys.
    [[nodiscard]]
    float Axis(const Frost::Input& input, const Frost::Key positive, const Frost::Key negative) {
        return (input.IsKeyDown(positive) ? 1.0f : 0.0f) - (input.IsKeyDown(negative) ? 1.0f : 0.0f);
    }

}  // namespace

namespace Frost {

    CharacterInput CharacterControllerComponent::PollInput(const Input& input) {
        CharacterInput result;
        result.move = glm::vec2(Axis(input, Key::D, Key::A), Axis(input, Key::W, Key::S));
        result.jump = input.WasKeyPressed(Key::Space);

        // The mouse is for looking only while it is captured; otherwise it belongs to the desktop
        // (or, later, to whatever has input priority).
        if (input.GetCursorMode() == CursorMode::Captured) { result.look = input.GetMouseDelta(); }
        return result;
    }

    void CharacterControllerComponent::Apply(const CharacterInput& input, TransformComponent& transform,
                                             RigidBodyComponent& body, CameraComponent* const camera) const {
        // Look: left/right turns the whole body, up/down only the camera. Screen +x is right and +y
        // down: moving the mouse right turns right, up looks up.
        transform.rotation.y = std::remainder(transform.rotation.y - input.look.x * lookSensitivity, 360.0f);
        if (camera != nullptr) {
            camera->rotation.x = std::clamp(camera->rotation.x - input.look.y * lookSensitivity, -kMaxPitch, kMaxPitch);
        }

        // Walk on the ground plane, relative to the way the body faces (its yaw only: looking up or
        // down does not make it walk into the ground or the sky). Diagonals are no faster.
        const glm::quat yaw = glm::angleAxis(glm::radians(transform.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 forward = yaw * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 right = yaw * glm::vec3(1.0f, 0.0f, 0.0f);

        glm::vec3 walk = right * input.move.x + forward * input.move.y;
        if (const float length = glm::length(walk); length > 1.0f) { walk /= length; }

        // The horizontal velocity is the walk (whatever it was: the character stops when the keys
        // are released, and steers in the air too); the vertical one is physics' (falling).
        body.linearVelocity.x = walk.x * walkSpeed;
        body.linearVelocity.z = walk.z * walkSpeed;

        if (input.jump && body.onGround) {
            body.linearVelocity.y = jumpSpeed;
            body.onGround = false;  // Airborne until physics finds ground under it again.
        }
    }

    void CharacterControllerComponent::Update(const Input& input, TransformComponent& transform,
                                              RigidBodyComponent& body, CameraComponent* const camera) const {
        Apply(PollInput(input), transform, body, camera);
    }

}  // namespace Frost
