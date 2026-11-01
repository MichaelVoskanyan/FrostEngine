#pragma once

#include <vulkan/vulkan.h>

#include <vector>

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

        // True if the instance was created with the surface-maintenance1 extension (KHR or EXT) and
        // its dependency. This is the instance-level half of swapchain-maintenance1 (presentation
        // fences); the device-level half is checked by VulkanPhysicalDevice. Optional: absence just
        // means the renderer falls back to fence-less presentation-completion tracking.
        [[nodiscard]]
        bool HasSurfaceMaintenance1() const noexcept;

    private:
        static void EnsureVulkan14Support();

        // Appends the optional instance extensions that are actually available, recording which
        // ones were enabled.
        void AppendOptionalInstanceExtensions(std::vector<const char*>& extensions);

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
        bool m_SurfaceMaintenance1 = false;
    };

}  // namespace Frost
