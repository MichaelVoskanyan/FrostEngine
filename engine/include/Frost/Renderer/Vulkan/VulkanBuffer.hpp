#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <memory>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace Frost {

    class VulkanAllocator;
    class VulkanDevice;
    class VulkanCommandPool;

    // RAII wrapper around a VkBuffer + its VmaAllocation.
    class VulkanBuffer final {
    public:
        // Creates a buffer backed by `allocator`. When `hostVisible` is true, the buffer is
        // persistently mapped and host-coherent (a staging buffer, populated via CopyData);
        // otherwise it is device-local, and can only be populated through CreateDeviceLocal's
        // staging upload.
        VulkanBuffer(const VulkanAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                    bool hostVisible);
        ~VulkanBuffer();

        VulkanBuffer(const VulkanBuffer&) = delete;
        VulkanBuffer& operator=(const VulkanBuffer&) = delete;

        VulkanBuffer(VulkanBuffer&&) = delete;
        VulkanBuffer& operator=(VulkanBuffer&&) = delete;

        [[nodiscard]]
        VkBuffer GetHandle() const noexcept;

        [[nodiscard]]
        VkDeviceSize GetSize() const noexcept;

        // Only valid on a buffer created with hostVisible = true.
        void CopyData(const void* data, std::size_t size) const;

        // Creates a device-local buffer of `size` bytes containing `data`, uploaded through a
        // temporary staging buffer and a one-off command buffer submitted via `commandPool`.
        [[nodiscard]]
        static std::unique_ptr<VulkanBuffer> CreateDeviceLocal(const VulkanAllocator& allocator,
                                                                const VulkanDevice& device,
                                                                const VulkanCommandPool& commandPool,
                                                                const void* data, VkDeviceSize size,
                                                                VkBufferUsageFlags usage);

    private:
        VmaAllocator m_AllocatorHandle = nullptr;  // Non-owning; the VulkanAllocator must outlive this buffer.
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        VkDeviceSize m_Size = 0;
        void* m_MappedData = nullptr;  // Non-null only for hostVisible buffers.
    };

}  // namespace Frost
