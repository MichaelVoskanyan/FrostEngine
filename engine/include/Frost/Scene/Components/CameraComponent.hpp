#pragma once

#include <Frost/Renderer/Camera.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>
#include <Frost/Units.hpp>

namespace Frost {

    // A viewpoint placed in a level: the game looks through it (see GameApplication).
    //
    // It is mounted on its object: it sits at `offset` from the object's position and is turned by
    // `rotation` on top of the object's own rotation, so it moves and turns with the object. With
    // neither (the default) it is exactly at the object and looks along its forward direction
    // (TransformComponent::GetForward), so placing and aiming a camera is moving and rotating its
    // object. A character carries its camera at head height and turns its body left and right, and
    // only the camera up and down (see CharacterControllerComponent). The object's scale is ignored.
    struct CameraComponent {
        static constexpr const char* kDisplayName = "Camera";

        float verticalFov = 60.0f;  // Degrees, top to bottom of the view.
        float nearClip = 5.0f * kCentimeter;  // Nothing closer than this is drawn.
        float farClip = 1.0f * kKilometer;    // Nothing further than this is drawn.

        // Where it sits relative to its object, in the object's space (it turns with the object but
        // is not scaled by it).
        glm::vec3 offset{0.0f};

        // Its own turn relative to its object: Euler angles in degrees, in the same convention as
        // TransformComponent::rotation (x = pitch, y = yaw, z = roll).
        glm::vec3 rotation{0.0f};

        // The camera as the renderer uses it, placed by `transform`. Out-of-range values (they come
        // from UI fields and level files) are brought into range rather than rejected.
        [[nodiscard]]
        Camera ToCamera(const TransformComponent& transform) const;
    };

}  // namespace Frost
