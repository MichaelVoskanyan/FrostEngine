#pragma once

#include <Frost/Renderer/Light.hpp>
#include <Frost/Units.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>

#include <glm/vec3.hpp>

namespace Frost {

    // A light attached to an object. It sits at the object's position and shines along the
    // object's forward direction (TransformComponent::GetForward), so placing and aiming a light is
    // just moving and rotating its object.
    struct LightComponent {
        static constexpr const char* kDisplayName = "Light";

        LightType type = LightType::Point;
        bool enabled = true;

        // Linear RGB tint. With useTemperature, multiplied by the black-body color of `temperature`.
        glm::vec3 color{1.0f};
        bool useTemperature = false;
        float temperature = 6500.0f;  // Kelvin.

        // Brightness. Directional: illuminance on a surface facing the light (3 lights a white
        // surface to about white). Point/spot: intensity at 1 meter, falling off with the inverse
        // square of the distance.
        float intensity = 20.0f;

        // Point: radius of influence. Spot: how far the cone reaches. Lighting fades smoothly to
        // zero at this distance.
        float range = 10.0f * kMeter;

        // Spot: cone half-angles in degrees. Full brightness inside the inner cone, fading to dark
        // at the outer one. 0 <= inner <= outer < 90.
        float innerConeAngle = 20.0f;
        float outerConeAngle = 30.0f;

        // Sensible starting values for a new light of each type.
        [[nodiscard]]
        static LightComponent CreateDefault(LightType type) noexcept;

        // The final linear color * intensity.
        [[nodiscard]]
        glm::vec3 GetRadiance() const noexcept;

        // The light as the renderer consumes it, placed by `transform`.
        [[nodiscard]]
        Light ToRenderLight(const TransformComponent& transform) const noexcept;
    };

    // Display/serialization name of a light type ("directional", "point", "spot").
    [[nodiscard]]
    const char* GetLightTypeName(LightType type) noexcept;

}  // namespace Frost
