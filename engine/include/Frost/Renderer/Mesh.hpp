#pragma once

#include <Frost/Renderer/Vertex.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace Frost {

    class VulkanAllocator;
    class VulkanDevice;
    class VulkanCommandPool;
    class VulkanBuffer;

    // A drawable: a device-local vertex buffer + index buffer pair, uploaded once at construction.
    // This is the base unit Renderer submits draw calls for; a model/asset system builds Meshes
    // from loaded geometry rather than replacing this type.
    class Mesh final {
    public:
        Mesh(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
             const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices);
        ~Mesh();

        Mesh(const Mesh&) = delete;
        Mesh& operator=(const Mesh&) = delete;

        Mesh(Mesh&&) = delete;
        Mesh& operator=(Mesh&&) = delete;

        // Binds this mesh's vertex/index buffers and issues an indexed draw call.
        void Draw(VkCommandBuffer commandBuffer) const;

    private:
        std::unique_ptr<VulkanBuffer> m_VertexBuffer;
        std::unique_ptr<VulkanBuffer> m_IndexBuffer;
        std::uint32_t m_IndexCount = 0;
    };

}  // namespace Frost
