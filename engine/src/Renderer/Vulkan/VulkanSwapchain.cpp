#include <Frost/Renderer/Vulkan/VulkanSwapchain.hpp>

#include <Frost/Platform/Window.hpp>
#include <Frost/Renderer/Vulkan/VulkanContext.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << '.';

        return std::runtime_error(message.str());
    }

}  // namespace

namespace Frost {

    VulkanSwapchain::VulkanSwapchain(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                                      const VulkanDevice& device, const Window& window)
        : m_Device(device.GetHandle()) {
        const SwapchainSupportDetails support = physicalDevice.QuerySwapchainSupport(context.GetSurface());
        const VkExtent2D extent = ChooseExtent(support.capabilities, window);

        if (extent.width == 0 || extent.height == 0) {
            throw std::runtime_error("Cannot create a swapchain for a zero-area window surface (is the window minimized?).");
        }

        CreateSwapchain(context, physicalDevice, support, extent, VK_NULL_HANDLE);
        CreateImageViews();
    }

    VulkanSwapchain::~VulkanSwapchain() {
        DestroyImageViews();

        if (m_Swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;
        }
    }

    bool VulkanSwapchain::Recreate(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                                   const Window& window, Retired& retired) {
        // Queried fresh: currentExtent tracks the window's current size, which is the point of a
        // recreate. It is also the authoritative size (the window's own framebuffer size may lag
        // behind the OS by a frame), so the zero-area check is made on it, and made before any
        // destructive step below.
        const SwapchainSupportDetails support = physicalDevice.QuerySwapchainSupport(context.GetSurface());
        const VkExtent2D extent = ChooseExtent(support.capabilities, window);

        if (extent.width == 0 || extent.height == 0) { return false; }

        // Nothing old is destroyed here: the views and images may still be read by in-flight
        // command buffers and the presentation engine. They are moved out to the caller, who
        // destroys them once that is provably no longer the case.
        Retired previous{.swapchain = m_Swapchain, .imageViews = std::move(m_ImageViews)};
        m_ImageViews.clear();

        try {
            CreateSwapchain(context, physicalDevice, support, extent, previous.swapchain);
        }
        catch (...) {
            // The old swapchain is retired by the failed call (Vulkan rule) but still owned by
            // this object; put its views back so the destructor tears everything down normally.
            //
            // Error-handling policy: a failed vkCreateSwapchainKHR is treated as fatal by the
            // renderer (the exception propagates and the process exits). This object is therefore
            // left in a destroy-only state on purpose: the retired handle must not be acquired
            // from, presented to, or passed as oldSwapchain again (VUID-VkSwapchainCreateInfoKHR-
            // oldSwapchain-01933). Recovering in-process would need a fresh swapchain built with
            // oldSwapchain = VK_NULL_HANDLE and, for VK_ERROR_SURFACE_LOST_KHR, a new surface.
            m_ImageViews = std::move(previous.imageViews);
            throw;
        }

        retired = std::move(previous);
        CreateImageViews();

        return true;
    }

    void VulkanSwapchain::DestroyRetired(const VkDevice device, Retired& retired) noexcept {
        for (const VkImageView imageView : retired.imageViews) {
            if (imageView != VK_NULL_HANDLE) { vkDestroyImageView(device, imageView, nullptr); }
        }

        retired.imageViews.clear();

        if (retired.swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, retired.swapchain, nullptr);
            retired.swapchain = VK_NULL_HANDLE;
        }
    }

    VkSwapchainKHR VulkanSwapchain::GetHandle() const noexcept { return m_Swapchain; }

    VkFormat VulkanSwapchain::GetImageFormat() const noexcept { return m_ImageFormat; }

    const VkExtent2D& VulkanSwapchain::GetExtent() const noexcept { return m_Extent; }

    const std::vector<VkImageView>& VulkanSwapchain::GetImageViews() const noexcept { return m_ImageViews; }

    const std::vector<VkImage>& VulkanSwapchain::GetImages() const noexcept { return m_Images; }

    void VulkanSwapchain::CreateSwapchain(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                                          const SwapchainSupportDetails& support, const VkExtent2D extent,
                                          const VkSwapchainKHR oldSwapchain) {
        const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.formats);
        const VkPresentModeKHR presentMode = ChoosePresentMode(support.presentModes);

        std::uint32_t imageCount = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
            imageCount = support.capabilities.maxImageCount;
        }

        const QueueFamilyIndices& indices = physicalDevice.GetQueueFamilyIndices();
        const std::array<std::uint32_t, 2> queueFamilyIndices = {indices.graphicsFamily.value(),
                                                                  indices.presentFamily.value()};
        const bool concurrentQueues = indices.graphicsFamily.value() != indices.presentFamily.value();

        const VkSwapchainCreateInfoKHR createInfo{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                .pNext = nullptr,
                .flags = 0,
                .surface = context.GetSurface(),
                .minImageCount = imageCount,
                .imageFormat = surfaceFormat.format,
                .imageColorSpace = surfaceFormat.colorSpace,
                .imageExtent = extent,
                .imageArrayLayers = 1,
                .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .imageSharingMode = concurrentQueues ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = concurrentQueues ? static_cast<std::uint32_t>(queueFamilyIndices.size()) : 0,
                .pQueueFamilyIndices = concurrentQueues ? queueFamilyIndices.data() : nullptr,
                .preTransform = support.capabilities.currentTransform,
                .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                .presentMode = presentMode,
                .clipped = VK_TRUE,
                .oldSwapchain = oldSwapchain,
        };

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;

        const VkResult result = vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &swapchain);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateSwapchainKHR", result); }

        // A non-null oldSwapchain is now retired (it can no longer be acquired from or presented
        // to) but is deliberately not destroyed here; see Recreate().
        m_Swapchain = swapchain;
        m_ImageFormat = surfaceFormat.format;
        m_Extent = extent;

        // Checked: m_Images feeds per-image resources (views, render-finished semaphores) and the
        // per-frame image-index lookup, so a silently wrong count would be an out-of-bounds access.
        std::uint32_t swapchainImageCount = 0;
        VkResult imagesResult = vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &swapchainImageCount, nullptr);
        if (imagesResult != VK_SUCCESS) { throw VulkanError("vkGetSwapchainImagesKHR", imagesResult); }

        m_Images.resize(swapchainImageCount);
        imagesResult = vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &swapchainImageCount, m_Images.data());
        if (imagesResult != VK_SUCCESS && imagesResult != VK_INCOMPLETE) {
            throw VulkanError("vkGetSwapchainImagesKHR", imagesResult);
        }
    }

    void VulkanSwapchain::DestroyImageViews() noexcept {
        for (const VkImageView imageView : m_ImageViews) {
            if (imageView != VK_NULL_HANDLE) { vkDestroyImageView(m_Device, imageView, nullptr); }
        }

        m_ImageViews.clear();
    }

    void VulkanSwapchain::CreateImageViews() {
        m_ImageViews.resize(m_Images.size(), VK_NULL_HANDLE);

        for (std::size_t i = 0; i < m_Images.size(); ++i) {
            const VkImageViewCreateInfo createInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .image = m_Images[i],
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = m_ImageFormat,
                    .components = {.r = VK_COMPONENT_SWIZZLE_IDENTITY,
                                   .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                                   .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                                   .a = VK_COMPONENT_SWIZZLE_IDENTITY},
                    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .baseMipLevel = 0,
                                        .levelCount = 1,
                                        .baseArrayLayer = 0,
                                        .layerCount = 1},
            };

            const VkResult result = vkCreateImageView(m_Device, &createInfo, nullptr, &m_ImageViews[i]);
            if (result != VK_SUCCESS) { throw VulkanError("vkCreateImageView", result); }
        }
    }

    VkSurfaceFormatKHR VulkanSwapchain::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const VkSurfaceFormatKHR& format : availableFormats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }

        return availableFormats.front();
    }

    VkPresentModeKHR VulkanSwapchain::ChoosePresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const VkPresentModeKHR mode : availablePresentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) { return mode; }
        }

        // VK_PRESENT_MODE_FIFO_KHR is required to be supported by the spec, so this is always safe.
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D VulkanSwapchain::ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, const Window& window) {
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        const auto [width, height] = window.GetFramebufferSize();

        VkExtent2D extent{.width = width, .height = height};
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height =
                std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        return extent;
    }

}  // namespace Frost
