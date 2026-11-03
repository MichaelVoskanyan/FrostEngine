#pragma once

#include <Frost/Assets/Image.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace Frost {

    class VulkanAllocator;
    class VulkanDevice;
    class VulkanCommandPool;
    class VulkanImage;

    enum class TextureFilter { Nearest, Linear };

    // How mip levels are selected. None: no mip chain is built and only the base level is sampled.
    enum class TextureMipmapMode { None, Nearest, Linear };

    enum class TextureWrap { Repeat, MirroredRepeat, ClampToEdge };

    // How a texture's pixels are sampled. The defaults (trilinear, repeat, sRGB) are right for a
    // typical color texture and match glTF's defaults for an unspecified sampler.
    struct TextureSettings {
        TextureFilter magFilter = TextureFilter::Linear;
        TextureFilter minFilter = TextureFilter::Linear;
        TextureMipmapMode mipmapMode = TextureMipmapMode::Linear;
        TextureWrap wrapU = TextureWrap::Repeat;
        TextureWrap wrapV = TextureWrap::Repeat;
        // True for color data authored in sRGB (base color / albedo, i.e. nearly every PNG/JPEG):
        // the hardware then converts to linear when sampling. False for data textures (normals,
        // roughness, ...), which must be read as stored.
        bool srgb = true;
    };

    // A 2D texture ready to be sampled by the graphics pipeline: a device-local RGBA8 image (with
    // a generated mip chain unless mipmapMode is None) and the sampler described by its settings.
    // Uploaded once at construction. Textures are bound through a Material's descriptor set and
    // shared between materials via std::shared_ptr.
    class Texture final {
    public:
        // The allocator and device must outlive the texture.
        Texture(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
                const ImageData& image, const TextureSettings& settings = {});
        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        Texture(Texture&&) = delete;
        Texture& operator=(Texture&&) = delete;

        [[nodiscard]]
        VkImageView GetImageView() const noexcept;

        [[nodiscard]]
        VkSampler GetSampler() const noexcept;

        [[nodiscard]]
        std::uint32_t GetWidth() const noexcept;

        [[nodiscard]]
        std::uint32_t GetHeight() const noexcept;

        [[nodiscard]]
        std::uint32_t GetMipLevels() const noexcept;

        [[nodiscard]]
        const TextureSettings& GetSettings() const noexcept;

    private:
        VkDevice m_Device = VK_NULL_HANDLE;  // Non-owning; used to destroy the sampler.
        TextureSettings m_Settings;
        std::unique_ptr<VulkanImage> m_Image;
        VkSampler m_Sampler = VK_NULL_HANDLE;
    };

}  // namespace Frost
