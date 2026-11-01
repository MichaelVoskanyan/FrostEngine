#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace Frost {

    class Window;
    class VulkanContext;
    class VulkanPhysicalDevice;
    class VulkanDevice;
    struct SwapchainSupportDetails;

    // Owns the VkSwapchainKHR for a VulkanContext's surface plus a VkImageView per swapchain image.
    // Swapchain images themselves are owned by the swapchain and are destroyed implicitly with it.
    //
    // Recreate() rebuilds the swapchain against the surface's current extent (window resize). It
    // does not destroy the previous swapchain/views: those are handed back as a Retired bundle,
    // because in-flight GPU work and the presentation engine may still reference them. The owner
    // destroys the bundle (DestroyRetired) once it has established that they do not, and refreshes
    // anything derived from GetImages()/GetImageViews()/GetExtent()/GetImageFormat().
    class VulkanSwapchain final {
    public:
        // A replaced swapchain and the views onto its images. Destroy with DestroyRetired() only
        // after (a) every command buffer that referenced the views/images has completed and (b)
        // the presentation engine has finished with presents issued on it.
        struct Retired {
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            std::vector<VkImageView> imageViews;
        };

        VulkanSwapchain(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                        const VulkanDevice& device, const Window& window);
        ~VulkanSwapchain();

        VulkanSwapchain(const VulkanSwapchain&) = delete;
        VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

        VulkanSwapchain(VulkanSwapchain&&) = delete;
        VulkanSwapchain& operator=(VulkanSwapchain&&) = delete;

        // Replaces the swapchain and its image views with ones matching the surface's current
        // size/capabilities. The old swapchain is handed to the driver as oldSwapchain so it can
        // reuse resources, and then returned (with its views) in `retired` for deferred destruction.
        //
        // Returns false, having changed nothing, if the surface currently has zero area (window
        // minimized, or collapsed to zero width/height): a zero-extent swapchain is invalid, and
        // because Vulkan retires oldSwapchain even when creation fails, the check has to happen
        // before anything is touched. The existing swapchain/views stay valid and the caller should
        // simply skip the frame and retry later.
        [[nodiscard]]
        bool Recreate(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                      const Window& window, Retired& retired);

        // Destroys a Retired bundle (views first, then the swapchain) and resets it to empty.
        static void DestroyRetired(VkDevice device, Retired& retired) noexcept;

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

        // Creates a new swapchain for `extent` (retiring `oldSwapchain`, if any, which the caller
        // keeps ownership of) and replaces m_Swapchain on success. On failure m_Swapchain is left
        // untouched so the destructor can still clean up. `support` and `extent` are passed in
        // (rather than queried here) so the caller can validate the extent before committing to
        // retiring the old swapchain.
        void CreateSwapchain(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                              const SwapchainSupportDetails& support, VkExtent2D extent,
                              VkSwapchainKHR oldSwapchain);
        void CreateImageViews();
        void DestroyImageViews() noexcept;

        // Non-owning; the VulkanDevice that created this swapchain must outlive it.
        VkDevice m_Device = VK_NULL_HANDLE;

        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        VkFormat m_ImageFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D m_Extent{};

        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;
    };

}  // namespace Frost
