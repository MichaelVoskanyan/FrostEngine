#include <Frost/Renderer/Texture.hpp>

#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanImage.hpp>

#include <sstream>
#include <stdexcept>

namespace {

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << '.';

        return std::runtime_error(message.str());
    }

    [[nodiscard]]
    VkFilter ToVkFilter(const Frost::TextureFilter filter) noexcept {
        return filter == Frost::TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }

    [[nodiscard]]
    VkSamplerAddressMode ToVkAddressMode(const Frost::TextureWrap wrap) noexcept {
        switch (wrap) {
            case Frost::TextureWrap::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case Frost::TextureWrap::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case Frost::TextureWrap::Repeat: break;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }

}  // namespace

namespace Frost {

    Texture::Texture(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
                     const ImageData& image, const TextureSettings& settings)
        : m_Device(device.GetHandle()), m_Settings(settings) {
        if (image.width == 0 || image.height == 0) { throw std::invalid_argument("Texture image has an empty size."); }

        const std::size_t expectedBytes = static_cast<std::size_t>(image.width) * image.height * 4;
        if (image.pixels.size() != expectedBytes) {
            throw std::invalid_argument("Texture image pixel data does not match its size (expected RGBA8).");
        }

        const VkFormat format = settings.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        const bool mipmaps = settings.mipmapMode != TextureMipmapMode::None;

        m_Image = VulkanImage::CreateTexture2D(allocator, device, commandPool, image.pixels.data(),
                                               VkExtent2D{.width = image.width, .height = image.height}, format,
                                               mipmaps);

        // With mipmapMode None the image has a single level; a NEAREST mipmap mode with lod clamped
        // to 0 then always reads it without any filtering between levels.
        const VkSamplerCreateInfo samplerInfo{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .magFilter = ToVkFilter(settings.magFilter),
                .minFilter = ToVkFilter(settings.minFilter),
                .mipmapMode = settings.mipmapMode == TextureMipmapMode::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                               : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = ToVkAddressMode(settings.wrapU),
                .addressModeV = ToVkAddressMode(settings.wrapV),
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                .mipLodBias = 0.0f,
                .anisotropyEnable = VK_FALSE,  // Requires the samplerAnisotropy device feature, not enabled yet.
                .maxAnisotropy = 1.0f,
                .compareEnable = VK_FALSE,
                .compareOp = VK_COMPARE_OP_ALWAYS,
                .minLod = 0.0f,
                .maxLod = mipmaps ? VK_LOD_CLAMP_NONE : 0.0f,
                .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
                .unnormalizedCoordinates = VK_FALSE,
        };

        const VkResult result = vkCreateSampler(m_Device, &samplerInfo, nullptr, &m_Sampler);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateSampler", result); }
    }

    Texture::~Texture() {
        if (m_Sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_Device, m_Sampler, nullptr);
            m_Sampler = VK_NULL_HANDLE;
        }
    }

    VkImageView Texture::GetImageView() const noexcept { return m_Image->GetView(); }

    VkSampler Texture::GetSampler() const noexcept { return m_Sampler; }

    std::uint32_t Texture::GetWidth() const noexcept { return m_Image->GetExtent().width; }

    std::uint32_t Texture::GetHeight() const noexcept { return m_Image->GetExtent().height; }

    std::uint32_t Texture::GetMipLevels() const noexcept { return m_Image->GetMipLevels(); }

    const TextureSettings& Texture::GetSettings() const noexcept { return m_Settings; }

}  // namespace Frost
