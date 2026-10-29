#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace Frost {

    class Window;
    class VulkanContext;
    class VulkanPhysicalDevice;
    class VulkanDevice;

    // Owns the VkSwapchainKHR for a VulkanContext's surface plus a VkImageView per swapchain image.
    // Swapchain images themselves are owned by the swapchain and are destroyed implicitly with it.
    //
    // There is no Recreate() yet: resizing/minimizing the window will currently leave the swapchain
    // stale until the next full Renderer rebuild. Resize handling lands with the frame loop.
    class VulkanSwapchain final {
    public:
        VulkanSwapchain(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                        const VulkanDevice& device, const Window& window);
        ~VulkanSwapchain();

        VulkanSwapchain(const VulkanSwapchain&) = delete;
        VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

        VulkanSwapchain(VulkanSwapchain&&) = delete;
        VulkanSwapchain& operator=(VulkanSwapchain&&) = delete;

        [[nodiscard]]
        VkSwapchainKHR GetHandle() const noexcept;

        [[nodiscard]]
        VkFormat GetImageFormat() const noexcept;

        [[nodiscard]]
        const VkExtent2D& GetExtent() const noexcept;

        [[nodiscard]]
        const std::vector<VkImageView>& GetImageViews() const noexcept;

        // Raw swapchain image handles, in the same order as GetImageViews(); needed for the
        // per-frame layout-transition barriers Renderer issues (barriers operate on VkImage, not
        // VkImageView).
        [[nodiscard]]
        const std::vector<VkImage>& GetImages() const noexcept;

    private:
        [[nodiscard]]
        static VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);

        [[nodiscard]]
        static VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);

        [[nodiscard]]
        static VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, const Window& window);

        void CreateSwapchain(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                              const Window& window);
        void CreateImageViews();

        // Non-owning; the VulkanDevice that created this swapchain must outlive it.
        VkDevice m_Device = VK_NULL_HANDLE;

        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        VkFormat m_ImageFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D m_Extent{};

        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;
    };

}  // namespace Frost
