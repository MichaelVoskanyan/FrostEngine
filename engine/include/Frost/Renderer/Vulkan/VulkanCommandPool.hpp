#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace Frost {

    class VulkanPhysicalDevice;
    class VulkanDevice;

    // Owns a single VkCommandPool on the graphics queue family. Provides per-frame primary command
    // buffers (one per frame in flight, for Renderer's draw loop) plus ImmediateSubmit for one-off
    // transfer work (buffer uploads), which is recorded, submitted, and waited on synchronously.
    class VulkanCommandPool final {
    public:
        VulkanCommandPool(const VulkanPhysicalDevice& physicalDevice, const VulkanDevice& device);
        ~VulkanCommandPool();

        VulkanCommandPool(const VulkanCommandPool&) = delete;
        VulkanCommandPool& operator=(const VulkanCommandPool&) = delete;

        VulkanCommandPool(VulkanCommandPool&&) = delete;
        VulkanCommandPool& operator=(VulkanCommandPool&&) = delete;

        [[nodiscard]]
        VkCommandPool GetHandle() const noexcept;

        // Allocates `count` primary command buffers from this pool.
        [[nodiscard]]
        std::vector<VkCommandBuffer> AllocateCommandBuffers(const VulkanDevice& device, std::uint32_t count) const;

        // Records `recordFn` into a temporary command buffer, submits it to the graphics queue, and
        // blocks until it completes. Intended for buffer/image uploads, not per-frame rendering.
        void ImmediateSubmit(const VulkanDevice& device,
                              const std::function<void(VkCommandBuffer)>& recordFn) const;

    private:
        VkDevice m_Device = VK_NULL_HANDLE;  // Non-owning; used to destroy the pool.
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
    };

}  // namespace Frost
