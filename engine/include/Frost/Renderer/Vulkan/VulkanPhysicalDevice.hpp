#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace Frost {

    class VulkanContext;

    struct QueueFamilyIndices {
        std::optional<std::uint32_t> graphicsFamily;
        std::optional<std::uint32_t> presentFamily;

        [[nodiscard]]
        bool IsComplete() const noexcept;
    };

    struct SwapchainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    // Selects a single suitable physical device (GPU) for a VulkanContext: one that exposes a
    // graphics queue family, a queue family that can present to the context's surface, and support
    // for the swapchain extension with at least one usable surface format/present mode.
    class VulkanPhysicalDevice final {
    public:
        explicit VulkanPhysicalDevice(const VulkanContext& context);

        [[nodiscard]]
        VkPhysicalDevice GetHandle() const noexcept;

        [[nodiscard]]
        const QueueFamilyIndices& GetQueueFamilyIndices() const noexcept;

        [[nodiscard]]
        const VkPhysicalDeviceProperties& GetProperties() const noexcept;

        [[nodiscard]]
        SwapchainSupportDetails QuerySwapchainSupport(VkSurfaceKHR surface) const;

        // Device extensions required by the logical device; shared with VulkanDevice so the
        // extensions checked here at selection time are exactly the ones enabled at device
        // creation time.
        [[nodiscard]]
        static const std::vector<const char*>& RequiredExtensions();

    private:
        [[nodiscard]]
        static std::vector<VkPhysicalDevice> EnumeratePhysicalDevices(VkInstance instance);

        [[nodiscard]]
        static bool IsDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface);

        [[nodiscard]]
        static QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);

        [[nodiscard]]
        static bool CheckExtensionSupport(VkPhysicalDevice device);

        // Verifies the device supports (and can have enabled) the Vulkan 1.3 dynamicRendering and
        // synchronization2 features that VulkanDevice enables and Renderer's frame loop relies on.
        [[nodiscard]]
        static bool CheckFeatureSupport(VkPhysicalDevice device);

        void SelectPhysicalDevice(const VulkanContext& context);

        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties m_Properties{};
        QueueFamilyIndices m_QueueFamilyIndices;
    };

}  // namespace Frost
