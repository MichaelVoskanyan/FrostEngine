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
        CreateSwapchain(context, physicalDevice, window);
        CreateImageViews();
    }

    VulkanSwapchain::~VulkanSwapchain() {
        for (const VkImageView imageView : m_ImageViews) { vkDestroyImageView(m_Device, imageView, nullptr); }
        m_ImageViews.clear();

        if (m_Swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;
        }
    }

    VkSwapchainKHR VulkanSwapchain::GetHandle() const noexcept { return m_Swapchain; }

    VkFormat VulkanSwapchain::GetImageFormat() const noexcept { return m_ImageFormat; }

    const VkExtent2D& VulkanSwapchain::GetExtent() const noexcept { return m_Extent; }

    const std::vector<VkImageView>& VulkanSwapchain::GetImageViews() const noexcept { return m_ImageViews; }

    const std::vector<VkImage>& VulkanSwapchain::GetImages() const noexcept { return m_Images; }

    void VulkanSwapchain::CreateSwapchain(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                                          const Window& window) {
        const SwapchainSupportDetails support = physicalDevice.QuerySwapchainSupport(context.GetSurface());

        const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.formats);
        const VkPresentModeKHR presentMode = ChoosePresentMode(support.presentModes);
        const VkExtent2D extent = ChooseExtent(support.capabilities, window);

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
                .oldSwapchain = VK_NULL_HANDLE,
        };

        const VkResult result = vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &m_Swapchain);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateSwapchainKHR", result); }

        m_ImageFormat = surfaceFormat.format;
        m_Extent = extent;

        std::uint32_t swapchainImageCount = 0;
        vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &swapchainImageCount, nullptr);

        m_Images.resize(swapchainImageCount);
        vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &swapchainImageCount, m_Images.data());
    }

    void VulkanSwapchain::CreateImageViews() {
        m_ImageViews.resize(m_Images.size());

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
