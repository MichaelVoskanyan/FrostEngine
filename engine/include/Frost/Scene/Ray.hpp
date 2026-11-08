#pragma once

#include <glm/vec3.hpp>

namespace Frost {

    // A half-line in world space: from `origin`, along `direction`. Used both by the editor's mesh
    // picking (Level::Raycast) and by physics raycasts (Frost/Physics/Raycast.hpp).
    struct Ray {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};  // Unit length.
    };

}  // namespace Frost
