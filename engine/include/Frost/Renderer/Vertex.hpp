#pragma once

#include <vulkan/vulkan.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace Frost {

    // The engine's baseline vertex format. Every mesh built through Mesh uses this layout; a
    // model/material system that needs a different layout (skinned meshes, compressed attributes,
    // etc.) can introduce additional formats alongside this one later.
    struct Vertex {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f};
        glm::vec2 uv{0.0f};
        glm::vec4 color{1.0f};

        [[nodiscard]]
        static VkVertexInputBindingDescription GetBindingDescription() noexcept;

        [[nodiscard]]
        static std::array<VkVertexInputAttributeDescription, 4> GetAttributeDescriptions() noexcept;
    };

}  // namespace Frost
