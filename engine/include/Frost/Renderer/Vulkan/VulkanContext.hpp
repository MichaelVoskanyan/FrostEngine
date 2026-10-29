#pragma once

#include <vulkan/vulkan.h>

namespace Frost {

    class Window;

    // Owns the VkInstance, the (Debug-build-only) validation debug messenger, and the VkSurfaceKHR
    // for a single window. This is the entry point into Vulkan; device/swapchain selection happen
    // in the classes layered on top of it (VulkanPhysicalDevice, VulkanDevice, VulkanSwapchain).
    class VulkanContext final {
    public:
        explicit VulkanContext(const Window& window);
        ~VulkanContext();

        VulkanContext(const VulkanContext&) = delete;
        VulkanContext& operator=(const VulkanContext&) = delete;

        VulkanContext(VulkanContext&&) = delete;
        VulkanContext& operator=(VulkanContext&&) = delete;

        [[nodiscard]]
        VkInstance GetInstance() const noexcept;

        [[nodiscard]]
        VkSurfaceKHR GetSurface() const noexcept;

    private:
        static void EnsureVulkan14Support();

        [[nodiscard]]
        static bool CheckValidationLayerSupport();

        [[nodiscard]]
        static VkDebugUtilsMessengerCreateInfoEXT DebugMessengerCreateInfo() noexcept;

        void CreateInstance();
        void SetupDebugMessenger();
        void DestroyDebugMessenger() noexcept;
        void CreateSurface(const Window& window);

        VkInstance m_Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    };

}  // namespace Frost
