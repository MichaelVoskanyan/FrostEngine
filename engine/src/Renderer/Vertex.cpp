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

    std::array<VkVertexInputAttributeDescription, Vertex::kAttributeCount> Vertex::GetAttributeDescriptions() noexcept {
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
                VkVertexInputAttributeDescription{.location = 4,
                                                  .binding = 0,
                                                  .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                                  .offset = offsetof(Vertex, tangent)},
        };
    }

    VkVertexInputBindingDescription SkinnedVertex::GetBindingDescription() noexcept {
        return VkVertexInputBindingDescription{
                .binding = kBinding,
                .stride = sizeof(SkinnedVertex),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
    }

    std::array<VkVertexInputAttributeDescription, SkinnedVertex::kAttributeCount> SkinnedVertex::GetAttributeDescriptions() noexcept {
        // R16G16B16A16_UINT and R32G32B32A32_SFLOAT are both mandatory vertex buffer formats.
        return {
                VkVertexInputAttributeDescription{.location = Vertex::kAttributeCount,
                                                  .binding = kBinding,
                                                  .format = VK_FORMAT_R16G16B16A16_UINT,
                                                  .offset = offsetof(SkinnedVertex, joints)},
                VkVertexInputAttributeDescription{.location = Vertex::kAttributeCount + 1,
                                                  .binding = kBinding,
                                                  .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                                  .offset = offsetof(SkinnedVertex, weights)},
        };
    }

}  // namespace Frost
