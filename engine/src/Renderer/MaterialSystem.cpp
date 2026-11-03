#include <Frost/Renderer/MaterialSystem.hpp>

#include <utility>

namespace Frost {

    MaterialSystem::MaterialSystem(const VulkanAllocator& allocator, const VulkanDevice& device,
                                   const VulkanCommandPool& commandPool,
                                   VulkanDescriptorAllocator& descriptorAllocator)
        : m_Allocator(allocator), m_Device(device), m_CommandPool(commandPool),
          m_DescriptorAllocator(descriptorAllocator) {
        // Neutral values for unset slots: white multiplies factors through unchanged, and the
        // flat normal (0, 0, 1) leaves the geometric normal alone. Both are data, not color, in
        // the sense that they must read back exactly, so they are stored linear without mips.
        const TextureSettings dataSettings{.mipmapMode = TextureMipmapMode::None, .srgb = false};

        m_WhiteTexture = CreateTexture(ImageData::SolidColor(1, 1, {255, 255, 255, 255}), dataSettings);
        m_FlatNormalTexture = CreateTexture(ImageData::SolidColor(1, 1, {128, 128, 255, 255}), dataSettings);

        m_DefaultMaterial = CreateMaterial(MaterialParameters{.metallicFactor = 0.0f, .roughnessFactor = 1.0f});
    }

    MaterialSystem::~MaterialSystem() = default;

    std::shared_ptr<Texture> MaterialSystem::CreateTexture(const ImageData& image,
                                                           const TextureSettings& settings) const {
        return std::make_shared<Texture>(m_Allocator, m_Device, m_CommandPool, image, settings);
    }

    std::shared_ptr<Material> MaterialSystem::CreateMaterial(const MaterialParameters& parameters,
                                                             MaterialTextures textures) const {
        if (textures.baseColor == nullptr) { textures.baseColor = m_WhiteTexture; }
        if (textures.metallicRoughness == nullptr) { textures.metallicRoughness = m_WhiteTexture; }
        if (textures.normal == nullptr) { textures.normal = m_FlatNormalTexture; }
        if (textures.occlusion == nullptr) { textures.occlusion = m_WhiteTexture; }
        if (textures.emissive == nullptr) { textures.emissive = m_WhiteTexture; }

        return std::make_shared<Material>(m_Allocator, m_Device, m_DescriptorAllocator, parameters,
                                          std::move(textures));
    }

    const std::shared_ptr<Material>& MaterialSystem::GetDefaultMaterial() const noexcept { return m_DefaultMaterial; }

    const std::shared_ptr<Texture>& MaterialSystem::GetWhiteTexture() const noexcept { return m_WhiteTexture; }

    const std::shared_ptr<Texture>& MaterialSystem::GetFlatNormalTexture() const noexcept {
        return m_FlatNormalTexture;
    }

}  // namespace Frost
