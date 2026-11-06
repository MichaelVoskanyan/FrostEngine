#include <Frost/Scene/Components/FlyControllerComponent.hpp>

#include <Frost/Platform/Input.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace {

    // Just short of straight up/down: at exactly +-90 degrees, yaw and "up" stop being meaningful.
    constexpr float kMaxPitch = 89.0f;

    // Each wheel notch scales the speed by this; multiplicative steps keep the wheel equally useful
    // for creeping up to something and for crossing a large level.
    constexpr float kSpeedStep = 1.2f;
    constexpr float kMinMoveSpeed = 5.0f * Frost::kCentimeter;
    constexpr float kMaxMoveSpeed = 500.0f * Frost::kMeter;

    constexpr float kMaxDeltaTime = 0.1f;

    constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

    // -1, 0 or +1 from a pair of opposing keys.
    [[nodiscard]]
    float Axis(const Frost::Input& input, const Frost::Key positive, const Frost::Key negative) {
        return (input.IsKeyDown(positive) ? 1.0f : 0.0f) - (input.IsKeyDown(negative) ? 1.0f : 0.0f);
    }

}  // namespace

namespace Frost {

    FlyInput FlyControllerComponent::PollInput(const Input& input) {
        FlyInput result;
        result.move = glm::vec3(Axis(input, Key::D, Key::A), Axis(input, Key::Space, Key::LeftControl),
                                Axis(input, Key::W, Key::S));
        result.fast = input.IsKeyDown(Key::LeftShift);

        // The mouse is for looking only while it is captured; otherwise it belongs to the
        // desktop (or, later, to whatever has input priority).
        if (input.GetCursorMode() == CursorMode::Captured) {
            result.look = input.GetMouseDelta();
            result.speedSteps = input.GetScrollDelta().y;
        }
        return result;
    }

    void FlyControllerComponent::Apply(const FlyInput& input, const float deltaTime, TransformComponent& transform) {
        // Turn. The transform's Euler angles are applied yaw (about world up) then pitch (about the
        // object's own right axis), which is exactly a flying camera's turn: they are its state.
        // Screen +x is right and +y down: moving the mouse right turns right, up looks up.
        if (input.look.x != 0.0f || input.look.y != 0.0f) {
            transform.rotation.y = std::remainder(transform.rotation.y - input.look.x * lookSensitivity, 360.0f);
            transform.rotation.x = std::clamp(transform.rotation.x - input.look.y * lookSensitivity, -kMaxPitch, kMaxPitch);
        }

        if (input.speedSteps != 0.0f) {
            moveSpeed = std::clamp(moveSpeed * std::pow(kSpeedStep, input.speedSteps), kMinMoveSpeed, kMaxMoveSpeed);
        }

        // Move, along where it faces now: forward and sideways follow the view (so looking up and
        // pressing W climbs), up/down is the world's.
        const glm::quat rotation = transform.GetRotation();
        const glm::vec3 direction = rotation * glm::vec3(1.0f, 0.0f, 0.0f) * input.move.x + kWorldUp * input.move.y +
                                    rotation * glm::vec3(0.0f, 0.0f, -1.0f) * input.move.z;

        // Normalized, so diagonals are no faster than a single axis; opposing keys cancel out.
        if (glm::dot(direction, direction) > 1e-6f && deltaTime > 0.0f) {
            const float speed = moveSpeed * (input.fast ? fastMultiplier : 1.0f);
            transform.position += glm::normalize(direction) * (speed * std::min(deltaTime, kMaxDeltaTime));
        }
    }

    void FlyControllerComponent::Update(const Input& input, const float deltaTime, TransformComponent& transform) {
        Apply(PollInput(input), deltaTime, transform);
    }

}  // namespace Frost
