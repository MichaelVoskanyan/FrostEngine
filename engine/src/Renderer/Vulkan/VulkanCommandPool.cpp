#include <Frost/Renderer/Vulkan/VulkanCommandPool.hpp>

#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp>

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

    VulkanCommandPool::VulkanCommandPool(const VulkanPhysicalDevice& physicalDevice, const VulkanDevice& device)
        : m_Device(device.GetHandle()) {
        const VkCommandPoolCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = physicalDevice.GetQueueFamilyIndices().graphicsFamily.value(),
        };

        const VkResult result = vkCreateCommandPool(m_Device, &createInfo, nullptr, &m_CommandPool);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateCommandPool", result); }
    }

    VulkanCommandPool::~VulkanCommandPool() {
        if (m_CommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
            m_CommandPool = VK_NULL_HANDLE;
        }
    }

    VkCommandPool VulkanCommandPool::GetHandle() const noexcept { return m_CommandPool; }

    std::vector<VkCommandBuffer> VulkanCommandPool::AllocateCommandBuffers(const VulkanDevice& device,
                                                                            const std::uint32_t count) const {
        std::vector<VkCommandBuffer> commandBuffers(count);

        const VkCommandBufferAllocateInfo allocInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = m_CommandPool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = count,
        };

        const VkResult result = vkAllocateCommandBuffers(device.GetHandle(), &allocInfo, commandBuffers.data());
        if (result != VK_SUCCESS) { throw VulkanError("vkAllocateCommandBuffers", result); }

        return commandBuffers;
    }

    void VulkanCommandPool::ImmediateSubmit(const VulkanDevice& device,
                                            const std::function<void(VkCommandBuffer)>& recordFn) const {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

        const VkCommandBufferAllocateInfo allocInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = m_CommandPool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
        };

        VkResult result = vkAllocateCommandBuffers(device.GetHandle(), &allocInfo, &commandBuffer);
        if (result != VK_SUCCESS) { throw VulkanError("vkAllocateCommandBuffers", result); }

        const VkCommandBufferBeginInfo beginInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                .pInheritanceInfo = nullptr,
        };

        result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            vkFreeCommandBuffers(device.GetHandle(), m_CommandPool, 1, &commandBuffer);
            throw VulkanError("vkBeginCommandBuffer", result);
        }

        recordFn(commandBuffer);

        result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS) {
            vkFreeCommandBuffers(device.GetHandle(), m_CommandPool, 1, &commandBuffer);
            throw VulkanError("vkEndCommandBuffer", result);
        }

        const VkSubmitInfo submitInfo{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = nullptr,
                .waitSemaphoreCount = 0,
                .pWaitSemaphores = nullptr,
                .pWaitDstStageMask = nullptr,
                .commandBufferCount = 1,
                .pCommandBuffers = &commandBuffer,
                .signalSemaphoreCount = 0,
                .pSignalSemaphores = nullptr,
        };

        // A one-off transfer during resource creation, not per-frame rendering: blocking on
        // vkQueueWaitIdle here (rather than a fence) keeps the API simple and is not a performance
        // concern at upload time.
        result = vkQueueSubmit(device.GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) { result = vkQueueWaitIdle(device.GetGraphicsQueue()); }

        vkFreeCommandBuffers(device.GetHandle(), m_CommandPool, 1, &commandBuffer);

        if (result != VK_SUCCESS) { throw VulkanError("vkQueueSubmit/vkQueueWaitIdle", result); }
    }

}  // namespace Frost
