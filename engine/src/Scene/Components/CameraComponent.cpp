#include <Frost/Scene/Components/CameraComponent.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace {

    constexpr float kMinFov = 1.0f;  // Degrees.
    constexpr float kMaxFov = 170.0f;
    constexpr float kMinNearClip = 0.01f * Frost::kCentimeter;

}  // namespace

namespace Frost {

    Camera CameraComponent::ToCamera(const TransformComponent& transform) const {
        // std::clamp would pass NaN through; these comparisons replace it with the defaults.
        const CameraComponent defaults;
        const float fov = std::isfinite(verticalFov) ? std::clamp(verticalFov, kMinFov, kMaxFov) : defaults.verticalFov;
        const float nearPlane = std::isfinite(nearClip) && nearClip >= kMinNearClip ? nearClip : defaults.nearClip;
        float farPlane = std::isfinite(farClip) ? farClip : defaults.farClip;
        if (!(farPlane > nearPlane * 1.001f)) { farPlane = nearPlane * 1000.0f; }

        // The mount: offset in the object's (rotated, unscaled) space, then its own turn on top of
        // the object's.
        const glm::quat objectRotation = transform.GetRotation();
        const glm::quat mountRotation =
                TransformComponent{.position = {}, .rotation = rotation, .scale = glm::vec3(1.0f)}.GetRotation();

        Camera camera;
        camera.SetPerspective(glm::radians(fov), nearPlane, farPlane);
        camera.SetPosition(transform.position + objectRotation * offset);
        camera.SetOrientation(objectRotation * mountRotation);
        return camera;
    }

}  // namespace Frost
