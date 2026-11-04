#include <Frost/Renderer/Vulkan/VulkanImage.hpp>

#include <Frost/Renderer/Vulkan/VulkanAllocator.hpp>
#include <Frost/Renderer/Vulkan/VulkanBuffer.hpp>
#include <Frost/Renderer/Vulkan/VulkanCommandPool.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace {

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << '.';

        return std::runtime_error(message.str());
    }

    // Records a layout transition for `levelCount` mip levels starting at `baseMipLevel`.
    void TransitionMipLevels(const VkCommandBuffer commandBuffer, const VkImage image, const std::uint32_t baseMipLevel,
                             const std::uint32_t levelCount, const VkImageLayout oldLayout,
                             const VkImageLayout newLayout, const VkPipelineStageFlags2 srcStageMask,
                             const VkAccessFlags2 srcAccessMask, const VkPipelineStageFlags2 dstStageMask,
                             const VkAccessFlags2 dstAccessMask) {
        const VkImageMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = srcStageMask,
                .srcAccessMask = srcAccessMask,
                .dstStageMask = dstStageMask,
                .dstAccessMask = dstAccessMask,
                .oldLayout = oldLayout,
                .newLayout = newLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .baseMipLevel = baseMipLevel,
                                     .levelCount = levelCount,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
        };

        const VkDependencyInfo dependencyInfo{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext = nullptr,
                .dependencyFlags = 0,
                .memoryBarrierCount = 0,
                .pMemoryBarriers = nullptr,
                .bufferMemoryBarrierCount = 0,
                .pBufferMemoryBarriers = nullptr,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &barrier,
        };

        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

    [[nodiscard]]
    VkDeviceSize BytesPerTexel(const VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB: return 4;
            default: throw std::invalid_argument("VulkanImage::CreateTexture2D: unsupported texel format.");
        }
    }

}  // namespace

namespace Frost {

    VulkanImage::VulkanImage(const VulkanAllocator& allocator, const VulkanDevice& device, const VkExtent2D extent,
                             const VkFormat format, const std::uint32_t mipLevels, const VkImageUsageFlags usage)
        : m_AllocatorHandle(allocator.GetHandle()),
          m_Device(device.GetHandle()),
          m_Format(format),
          m_Extent(extent),
          m_MipLevels(mipLevels) {
        if (extent.width == 0 || extent.height == 0 || mipLevels == 0) {
            throw std::invalid_argument("VulkanImage requires a non-zero extent and mip level count.");
        }

        const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                .mipLevels = mipLevels,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        VkResult result = vmaCreateImage(m_AllocatorHandle, &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr);
        if (result != VK_SUCCESS) { throw VulkanError("vmaCreateImage", result); }

        const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = m_Image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = format,
                .components = {.r = VK_COMPONENT_SWIZZLE_IDENTITY,
                               .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                               .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                               .a = VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {.aspectMask = AspectMaskFor(format),
                                     .baseMipLevel = 0,
                                     .levelCount = mipLevels,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
        };

        result = vkCreateImageView(m_Device, &viewInfo, nullptr, &m_View);
        if (result != VK_SUCCESS) {
            // The destructor will not run for a partially constructed object.
            vmaDestroyImage(m_AllocatorHandle, m_Image, m_Allocation);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = nullptr;
            throw VulkanError("vkCreateImageView", result);
        }
    }

    VulkanImage::~VulkanImage() {
        if (m_View != VK_NULL_HANDLE) {
            vkDestroyImageView(m_Device, m_View, nullptr);
            m_View = VK_NULL_HANDLE;
        }

        if (m_Image != VK_NULL_HANDLE) {
            vmaDestroyImage(m_AllocatorHandle, m_Image, m_Allocation);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = nullptr;
        }
    }

    bool VulkanImage::IsDepthFormat(const VkFormat format) noexcept {
        switch (format) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
            default: return false;
        }
    }

    VkImageAspectFlags VulkanImage::AspectMaskFor(const VkFormat format) noexcept {
        // The depth aspect only, even for combined depth/stencil formats: the view is used as a
        // depth attachment, and the engine does not use stencil.
        return IsDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }

    VkImage VulkanImage::GetHandle() const noexcept { return m_Image; }

    VkImageView VulkanImage::GetView() const noexcept { return m_View; }

    VkFormat VulkanImage::GetFormat() const noexcept { return m_Format; }

    VkExtent2D VulkanImage::GetExtent() const noexcept { return m_Extent; }

    std::uint32_t VulkanImage::GetMipLevels() const noexcept { return m_MipLevels; }

    std::uint32_t VulkanImage::FullMipChainLevels(const VkExtent2D extent) noexcept {
        std::uint32_t levels = 1;
        for (std::uint32_t size = std::max(extent.width, extent.height); size > 1; size /= 2) { ++levels; }
        return levels;
    }

    std::unique_ptr<VulkanImage> VulkanImage::CreateTexture2D(const VulkanAllocator& allocator,
                                                              const VulkanDevice& device,
                                                              const VulkanCommandPool& commandPool,
                                                              const void* const pixels, const VkExtent2D extent,
                                                              const VkFormat format, const bool generateMipmaps) {
        const std::uint32_t mipLevels = generateMipmaps ? FullMipChainLevels(extent) : 1;

        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (mipLevels > 1) { usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; }  // Each level is blitted from the previous.

        auto image = std::make_unique<VulkanImage>(allocator, device, extent, format, mipLevels, usage);

        const VkDeviceSize byteCount = BytesPerTexel(format) * extent.width * extent.height;
        const VulkanBuffer staging(allocator, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        staging.CopyData(pixels, static_cast<std::size_t>(byteCount));

        // ImmediateSubmit blocks until the GPU has finished, so the staging buffer (a local) is
        // safe to destroy on return.
        commandPool.ImmediateSubmit(device, [&](const VkCommandBuffer commandBuffer) {
            const VkImage handle = image->GetHandle();

            // Whole chain: undefined -> transfer destination, ready for the copy (and the blits).
            TransitionMipLevels(commandBuffer, handle, 0, mipLevels, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            const VkBufferImageCopy copyRegion{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,  // Tightly packed.
                    .bufferImageHeight = 0,
                    .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1},
            };
            vkCmdCopyBufferToImage(commandBuffer, staging.GetHandle(), handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copyRegion);

            // Each level i is produced by a linear downsample blit from level i-1. Level i-1 goes
            // transfer-dst (written by the copy or the previous blit) -> transfer-src for the blit,
            // then straight to its final shader-read layout.
            std::int32_t sourceWidth = static_cast<std::int32_t>(extent.width);
            std::int32_t sourceHeight = static_cast<std::int32_t>(extent.height);

            for (std::uint32_t level = 1; level < mipLevels; ++level) {
                const std::int32_t destinationWidth = std::max(sourceWidth / 2, 1);
                const std::int32_t destinationHeight = std::max(sourceHeight / 2, 1);

                TransitionMipLevels(commandBuffer, handle, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                    VK_ACCESS_2_TRANSFER_READ_BIT);

                const VkImageBlit blit{
                        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                           .mipLevel = level - 1,
                                           .baseArrayLayer = 0,
                                           .layerCount = 1},
                        .srcOffsets = {{0, 0, 0}, {sourceWidth, sourceHeight, 1}},
                        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                           .mipLevel = level,
                                           .baseArrayLayer = 0,
                                           .layerCount = 1},
                        .dstOffsets = {{0, 0, 0}, {destinationWidth, destinationHeight, 1}},
                };
                vkCmdBlitImage(commandBuffer, handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                TransitionMipLevels(commandBuffer, handle, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                    VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

                sourceWidth = destinationWidth;
                sourceHeight = destinationHeight;
            }

            // The last level (or the only one) was written but never read as a blit source.
            TransitionMipLevels(commandBuffer, handle, mipLevels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });

        return image;
    }

}  // namespace Frost
