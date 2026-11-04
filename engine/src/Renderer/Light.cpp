#include <Frost/Renderer/Light.hpp>

#include <algorithm>
#include <cmath>

namespace {

    [[nodiscard]]
    float SrgbToLinear(const float value) noexcept {
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    }

}  // namespace

namespace Frost {

    glm::vec3 ColorTemperatureToLinearRgb(const float kelvin) noexcept {
        // Tanner Helland's fit of the black-body locus (sRGB-encoded, 0-255 per channel), valid for
        // 1000-40000 K. Accurate to within what matters for picking a light's tint.
        const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;

        float red = 255.0f;
        float green = 0.0f;
        float blue = 255.0f;

        if (t <= 66.0f) {
            green = 99.4708025861f * std::log(t) - 161.1195681661f;
            blue = t <= 19.0f ? 0.0f : 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f;
        }
        else {
            red = 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
            green = 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);
        }

        glm::vec3 linear(SrgbToLinear(std::clamp(red, 0.0f, 255.0f) / 255.0f),
                         SrgbToLinear(std::clamp(green, 0.0f, 255.0f) / 255.0f),
                         SrgbToLinear(std::clamp(blue, 0.0f, 255.0f) / 255.0f));

        // Tint only: the light's intensity carries the brightness.
        const float brightest = std::max({linear.x, linear.y, linear.z});
        return brightest > 0.0f ? linear / brightest : glm::vec3(1.0f);
    }

}  // namespace Frost
