#include <Frost/Renderer/Vertex.hpp>

#include <cstddef>

namespace Frost {

    VkVertexInputBindingDescription Vertex::GetBindingDescription() noexcept {
        return VkVertexInputBindingDescription{
                .binding = 0,
                .stride = sizeof(Vertex),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
    }

    std::array<VkVertexInputAttributeDescription, 4> Vertex::GetAttributeDescriptions() noexcept {
        return {
                VkVertexInputAttributeDescription{.location = 0,
                                                  .binding = 0,
                                                  .format = VK_FORMAT_R32G32B32_SFLOAT,
                                                  .offset = offsetof(Vertex, position)},
                VkVertexInputAttributeDescription{.location = 1,
                                                  .binding = 0,
                                                  .format = VK_FORMAT_R32G32B32_SFLOAT,
                                                  .offset = offsetof(Vertex, normal)},
                VkVertexInputAttributeDescription{.location = 2,
                                                  .binding = 0,
                                                  .format = VK_FORMAT_R32G32_SFLOAT,
                                                  .offset = offsetof(Vertex, uv)},
                VkVertexInputAttributeDescription{.location = 3,
                                                  .binding = 0,
                                                  .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                                  .offset = offsetof(Vertex, color)},
        };
    }

}  // namespace Frost
