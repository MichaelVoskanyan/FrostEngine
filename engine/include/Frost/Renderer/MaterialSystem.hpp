#pragma once

#include <Frost/Assets/Image.hpp>
#include <Frost/Renderer/Material.hpp>
#include <Frost/Renderer/Texture.hpp>

#include <memory>

namespace Frost {

    class VulkanAllocator;
    class VulkanDevice;
    class VulkanCommandPool;
    class VulkanDescriptorAllocator;

    // Creates the engine's textures and materials and owns the defaults they fall back on: a 1x1
    // white texture, a 1x1 flat normal texture, and the default material (matte white) used for
    // geometry that has no material assigned. This is the one place that knows which Vulkan
    // objects a Texture/Material needs, so a model loader or gameplay code can build materials -
    // from imported MeshData or by hand from separately imported images - without touching them.
    class MaterialSystem final {
    public:
        // All four must outlive the system and everything it creates.
        MaterialSystem(const VulkanAllocator& allocator, const VulkanDevice& device,
                       const VulkanCommandPool& commandPool, VulkanDescriptorAllocator& descriptorAllocator);
        ~MaterialSystem();

        MaterialSystem(const MaterialSystem&) = delete;
        MaterialSystem& operator=(const MaterialSystem&) = delete;

        MaterialSystem(MaterialSystem&&) = delete;
        MaterialSystem& operator=(MaterialSystem&&) = delete;

        // Uploads an image (from ImageImporter, or built in memory) as a texture.
        [[nodiscard]]
        std::shared_ptr<Texture> CreateTexture(const ImageData& image, const TextureSettings& settings = {}) const;

        // Builds a material from parameters and any subset of texture slots; unset slots are bound
        // to the neutral defaults, so `CreateMaterial({})` is a plain glTF-default material.
        [[nodiscard]]
        std::shared_ptr<Material> CreateMaterial(const MaterialParameters& parameters,
                                                 MaterialTextures textures = {}) const;

        // Bound for sub-meshes without a material: matte white (base color 1, metallic 0,
        // roughness 1). Deliberately not glTF's fully-metallic default, which has no diffuse term
        // and so is nearly invisible without environment lighting.
        [[nodiscard]]
        const std::shared_ptr<Material>& GetDefaultMaterial() const noexcept;

        [[nodiscard]]
        const std::shared_ptr<Texture>& GetWhiteTexture() const noexcept;

        [[nodiscard]]
        const std::shared_ptr<Texture>& GetFlatNormalTexture() const noexcept;

    private:
        const VulkanAllocator& m_Allocator;
        const VulkanDevice& m_Device;
        const VulkanCommandPool& m_CommandPool;
        VulkanDescriptorAllocator& m_DescriptorAllocator;

        std::shared_ptr<Texture> m_WhiteTexture;
        std::shared_ptr<Texture> m_FlatNormalTexture;
        std::shared_ptr<Material> m_DefaultMaterial;
    };

}  // namespace Frost
