#include <Frost/Renderer/Vulkan/VulkanBuffer.hpp>

#include <Frost/Renderer/Vulkan/VulkanAllocator.hpp>
#include <Frost/Renderer/Vulkan/VulkanCommandPool.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>

#include <vk_mem_alloc.h>

#include <cstring>
#include <sstream>
#include <stdexcept>

namespace {

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << '.';

        return std::runtime_error(message.str());
    }

}  // namespace

namespace Frost {

    VulkanBuffer::VulkanBuffer(const VulkanAllocator& allocator, const VkDeviceSize size,
                               const VkBufferUsageFlags usage, const bool hostVisible)
        : m_AllocatorHandle(allocator.GetHandle()), m_Size(size) {
        const VkBufferCreateInfo bufferInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = size,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
        };

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        if (hostVisible) {
            allocInfo.flags =
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        VmaAllocationInfo resultInfo{};

        const VkResult result =
                vmaCreateBuffer(m_AllocatorHandle, &bufferInfo, &allocInfo, &m_Buffer, &m_Allocation, &resultInfo);
        if (result != VK_SUCCESS) { throw VulkanError("vmaCreateBuffer", result); }

        m_MappedData = resultInfo.pMappedData;
    }

    VulkanBuffer::~VulkanBuffer() {
        if (m_Buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_AllocatorHandle, m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = nullptr;
        }
    }

    VkBuffer VulkanBuffer::GetHandle() const noexcept { return m_Buffer; }

    VkDeviceSize VulkanBuffer::GetSize() const noexcept { return m_Size; }

    void VulkanBuffer::CopyData(const void* const data, const std::size_t size) const {
        if (m_MappedData == nullptr) {
            throw std::runtime_error("VulkanBuffer::CopyData called on a buffer that is not host-visible.");
        }

        std::memcpy(m_MappedData, data, size);

        // VMA guarantees the mapping is HOST_VISIBLE, not HOST_COHERENT. Without a flush the
        // written bytes can sit in host caches and the GPU-side copy reads stale memory, so the
        // upload silently produces empty geometry. For coherent memory types this is a no-op.
        const VkResult result = vmaFlushAllocation(m_AllocatorHandle, m_Allocation, 0, VK_WHOLE_SIZE);
        if (result != VK_SUCCESS) { throw VulkanError("vmaFlushAllocation", result); }
    }

    std::unique_ptr<VulkanBuffer> VulkanBuffer::CreateDeviceLocal(const VulkanAllocator& allocator,
                                                                   const VulkanDevice& device,
                                                                   const VulkanCommandPool& commandPool,
                                                                   const void* const data, const VkDeviceSize size,
                                                                   const VkBufferUsageFlags usage) {
        const VulkanBuffer staging(allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        staging.CopyData(data, static_cast<std::size_t>(size));

        auto destination =
                std::make_unique<VulkanBuffer>(allocator, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);

        commandPool.ImmediateSubmit(device, [&](const VkCommandBuffer commandBuffer) {
            const VkBufferCopy copyRegion{.srcOffset = 0, .dstOffset = 0, .size = size};
            vkCmdCopyBuffer(commandBuffer, staging.GetHandle(), destination->GetHandle(), 1, &copyRegion);
        });

        return destination;
    }

}  // namespace Frost
