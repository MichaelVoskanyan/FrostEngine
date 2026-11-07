#include <Frost/Scene/Components/LightComponent.hpp>

#include <Frost/Renderer/Light.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace Frost {

    LightComponent LightComponent::CreateDefault(const LightType type) noexcept {
        LightComponent light;
        light.type = type;

        switch (type) {
            case LightType::Directional: light.intensity = 3.0f; break;
            case LightType::Point:
                light.intensity = 20.0f;
                light.range = 10.0f * kMeter;
                break;
            case LightType::Spot:
                light.intensity = 60.0f;
                light.range = 15.0f * kMeter;
                light.innerConeAngle = 20.0f;
                light.outerConeAngle = 30.0f;
                break;
        }

        return light;
    }

    glm::vec3 LightComponent::GetRadiance() const noexcept {
        const glm::vec3 tint = useTemperature ? color * ColorTemperatureToLinearRgb(temperature) : color;
        return tint * std::max(intensity, 0.0f);
    }

    Light LightComponent::ToRenderLight(const TransformComponent& transform) const noexcept {
        return ToRenderLight(transform.position, transform.GetRotation());
    }

    Light LightComponent::ToRenderLight(const glm::vec3& position, const glm::quat& rotation) const noexcept {
        // Sanitized here rather than trusted: values come from UI fields and level files.
        const float outer = std::clamp(outerConeAngle, 0.1f, 89.9f);
        const float inner = std::clamp(innerConeAngle, 0.0f, outer);

        return Light{
                .type = type,
                .position = position,
                .direction = glm::normalize(rotation * glm::vec3(0.0f, 0.0f, -1.0f)),
                .color = GetRadiance(),
                .range = std::max(range, 1.0f * kCentimeter),
                .innerConeCos = std::cos(glm::radians(inner)),
                .outerConeCos = std::cos(glm::radians(outer)),
        };
    }

    const char* GetLightTypeName(const LightType type) noexcept {
        switch (type) {
            case LightType::Directional: return "directional";
            case LightType::Point: return "point";
            case LightType::Spot: return "spot";
        }
        return "unknown";
    }

}  // namespace Frost
