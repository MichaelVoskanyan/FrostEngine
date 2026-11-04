#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace Frost {

    class VulkanAllocator;
    class VulkanDevice;
    class VulkanCommandPool;

    // RAII wrapper around a device-local 2D VkImage, its VmaAllocation and a VkImageView covering
    // every mip level. The constructor only allocates; CreateTexture2D is the upload path textures
    // use. Depth formats are supported too (the view then covers the depth aspect), for the
    // renderer's depth buffer.
    class VulkanImage final {
    public:
        VulkanImage(const VulkanAllocator& allocator, const VulkanDevice& device, VkExtent2D extent, VkFormat format,
                    std::uint32_t mipLevels, VkImageUsageFlags usage);
        ~VulkanImage();

        VulkanImage(const VulkanImage&) = delete;
        VulkanImage& operator=(const VulkanImage&) = delete;

        VulkanImage(VulkanImage&&) = delete;
        VulkanImage& operator=(VulkanImage&&) = delete;

        [[nodiscard]]
        VkImage GetHandle() const noexcept;

        [[nodiscard]]
        VkImageView GetView() const noexcept;

        [[nodiscard]]
        VkFormat GetFormat() const noexcept;

        [[nodiscard]]
        VkExtent2D GetExtent() const noexcept;

        [[nodiscard]]
        std::uint32_t GetMipLevels() const noexcept;

        [[nodiscard]]
        static bool IsDepthFormat(VkFormat format) noexcept;

        // The aspect an image of this format is viewed/transitioned with: DEPTH for depth formats
        // (stencil is not used), COLOR otherwise.
        [[nodiscard]]
        static VkImageAspectFlags AspectMaskFor(VkFormat format) noexcept;

        // Number of mip levels in a full chain for the given size (1 for a 1x1 image).
        [[nodiscard]]
        static std::uint32_t FullMipChainLevels(VkExtent2D extent) noexcept;

        // Creates a sampled 2D image from tightly packed pixels of `format` (4 bytes per texel for
        // the RGBA8 formats textures use), uploaded through a staging buffer and a one-off command
        // buffer. With `generateMipmaps`, the full mip chain is built on the GPU with linear blits
        // (the caller must only use formats that support linear-filtered blits; the RGBA8 UNORM /
        // SRGB formats are guaranteed to). The image is left in SHADER_READ_ONLY_OPTIMAL.
        [[nodiscard]]
        static std::unique_ptr<VulkanImage> CreateTexture2D(const VulkanAllocator& allocator,
                                                            const VulkanDevice& device,
                                                            const VulkanCommandPool& commandPool, const void* pixels,
                                                            VkExtent2D extent, VkFormat format, bool generateMipmaps);

    private:
        VmaAllocator m_AllocatorHandle = nullptr;  // Non-owning; the VulkanAllocator must outlive this image.
        VkDevice m_Device = VK_NULL_HANDLE;        // Non-owning; used to destroy the view.
        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        VkImageView m_View = VK_NULL_HANDLE;
        VkFormat m_Format = VK_FORMAT_UNDEFINED;
        VkExtent2D m_Extent{0, 0};
        std::uint32_t m_MipLevels = 1;
    };

}  // namespace Frost
