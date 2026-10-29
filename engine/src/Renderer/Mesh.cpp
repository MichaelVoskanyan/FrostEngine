#include <Frost/Renderer/Mesh.hpp>

#include <Frost/Renderer/Vulkan/VulkanBuffer.hpp>

#include <array>

namespace Frost {

    Mesh::Mesh(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
              const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices)
        : m_IndexCount(static_cast<std::uint32_t>(indices.size())) {
        const VkDeviceSize vertexBufferSize = sizeof(Vertex) * vertices.size();
        const VkDeviceSize indexBufferSize = sizeof(std::uint32_t) * indices.size();

        m_VertexBuffer = VulkanBuffer::CreateDeviceLocal(allocator, device, commandPool, vertices.data(),
                                                          vertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        m_IndexBuffer = VulkanBuffer::CreateDeviceLocal(allocator, device, commandPool, indices.data(),
                                                        indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    Mesh::~Mesh() = default;

    void Mesh::Draw(const VkCommandBuffer commandBuffer) const {
        const std::array<VkBuffer, 1> vertexBuffers = {m_VertexBuffer->GetHandle()};
        const std::array<VkDeviceSize, 1> offsets = {0};

        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers.data(), offsets.data());
        vkCmdBindIndexBuffer(commandBuffer, m_IndexBuffer->GetHandle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commandBuffer, m_IndexCount, 1, 0, 0, 0);
    }

}  // namespace Frost
