#pragma once

#include <glm/vec3.hpp>

#include <cstdint>

namespace Frost {

    enum class LightType : std::uint8_t {
        Directional,  // Parallel rays from infinitely far away (the sun); only its direction matters.
        Point,        // Radiates equally in all directions from a position, out to its range.
        Spot,         // A cone from a position along a direction, out to its range.
    };

    // A light as the renderer consumes it: world space, fully resolved (see Renderer::SubmitLight).
    // Scene code describes lights with LightComponent (Frost/Scene/GameObject.hpp) and converts.
    struct Light {
        LightType type = LightType::Point;

        glm::vec3 position{0.0f};                // World space. Ignored for directional lights.
        glm::vec3 direction{0.0f, -1.0f, 0.0f};  // World space, unit length: the way the light travels.

        // Linear RGB radiant intensity (color * intensity). Directional: the illuminance a surface
        // facing the light receives. Point/spot: the intensity at 1 unit from the light, falling off
        // with the inverse square of the distance.
        glm::vec3 color{1.0f};

        // Point/spot: distance at which the light has smoothly faded to nothing. Ignored for
        // directional lights.
        float range = 10.0f;

        // Spot: cosines of the cone's half-angles. Full intensity inside the inner cone, fading to
        // nothing at the outer cone. innerConeCos >= outerConeCos.
        float innerConeCos = 0.94f;
        float outerConeCos = 0.87f;
    };

    // Approximate linear-sRGB color of a black body at `kelvin` (1000-40000 K), normalized so its
    // brightest channel is 1. 6500 K is close to white; lower is warmer (orange), higher cooler.
    [[nodiscard]]
    glm::vec3 ColorTemperatureToLinearRgb(float kelvin) noexcept;

}  // namespace Frost
