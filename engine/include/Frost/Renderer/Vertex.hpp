#pragma once

#include <vulkan/vulkan.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>

namespace Frost {

    // The engine's baseline vertex format. Every mesh built through Mesh uses this layout; a
    // model/material system that needs a different layout (skinned meshes, compressed attributes,
    // etc.) can introduce additional formats alongside this one later.
    struct Vertex {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f};
        glm::vec2 uv{0.0f};
        glm::vec4 color{1.0f};
        // Tangent-space basis for normal mapping: xyz = tangent (the surface direction along which
        // u increases), w = +1/-1 handedness of the bitangent (bitangent = cross(normal, tangent) *
        // w), as in glTF. Zero xyz means "unknown"; the shader then skips normal mapping.
        glm::vec4 tangent{0.0f};

        static constexpr std::uint32_t kAttributeCount = 5;

        [[nodiscard]]
        static VkVertexInputBindingDescription GetBindingDescription() noexcept;

        [[nodiscard]]
        static std::array<VkVertexInputAttributeDescription, kAttributeCount> GetAttributeDescriptions() noexcept;
    };

}  // namespace Frost
