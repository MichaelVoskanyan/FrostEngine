#pragma once

#include <glm/common.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace Frost {

    // Axis-aligned bounding box.
    struct Bounds {
        glm::vec3 min{0.0f};
        glm::vec3 max{0.0f};

        [[nodiscard]]
        glm::vec3 GetCenter() const noexcept {
            return (min + max) * 0.5f;
        }

        [[nodiscard]]
        glm::vec3 GetSize() const noexcept {
            return max - min;
        }

        // The axis-aligned box enclosing this box after `transform` (all eight corners).
        [[nodiscard]]
        Bounds Transformed(const glm::mat4& transform) const noexcept {
            Bounds result;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 local((corner & 1) != 0 ? max.x : min.x, (corner & 2) != 0 ? max.y : min.y,
                                      (corner & 4) != 0 ? max.z : min.z);
                const glm::vec3 world = glm::vec3(transform * glm::vec4(local, 1.0f));

                result.min = corner == 0 ? world : glm::min(result.min, world);
                result.max = corner == 0 ? world : glm::max(result.max, world);
            }
            return result;
        }
    };

}  // namespace Frost
