#pragma once

#include <vulkan/vulkan.h>

namespace Frost {

    class VulkanPhysicalDevice;

    // Owns the logical VkDevice created for a VulkanPhysicalDevice, along with the graphics and
    // present VkQueue handles retrieved from it. Graphics and present may resolve to the same
    // underlying queue family; callers should not assume they differ.
    class VulkanDevice final {
    public:
        explicit VulkanDevice(const VulkanPhysicalDevice& physicalDevice);
        ~VulkanDevice();

        VulkanDevice(const VulkanDevice&) = delete;
        VulkanDevice& operator=(const VulkanDevice&) = delete;

        VulkanDevice(VulkanDevice&&) = delete;
        VulkanDevice& operator=(VulkanDevice&&) = delete;

        [[nodiscard]]
        VkDevice GetHandle() const noexcept;

        [[nodiscard]]
        VkQueue GetGraphicsQueue() const noexcept;

        [[nodiscard]]
        VkQueue GetPresentQueue() const noexcept;

        // True if swapchain-maintenance1 (KHR or EXT) was enabled on this device, i.e. presentation
        // fences (VkSwapchainPresentFenceInfoEXT) may be attached to vkQueuePresentKHR.
        [[nodiscard]]
        bool HasSwapchainMaintenance1() const noexcept;

    private:
        void CreateLogicalDevice(const VulkanPhysicalDevice& physicalDevice);

        VkDevice m_Device = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        VkQueue m_PresentQueue = VK_NULL_HANDLE;
        bool m_SwapchainMaintenance1 = false;
    };

}  // namespace Frost
