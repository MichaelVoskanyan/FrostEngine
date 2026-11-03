#pragma once

#include <Frost/Renderer/Texture.hpp>

#include <vulkan/vulkan.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Frost {

    class VulkanAllocator;
    class VulkanBuffer;
    class VulkanDevice;
    class VulkanDescriptorAllocator;

    // The scalar/vector inputs of the engine's surface model, which is glTF's metallic-roughness
    // model. Defaults are glTF's defaults, so an imported material only overrides what its file
    // specifies. Each texture input (see MaterialTextures) multiplies the matching factor.
    struct MaterialParameters {
        glm::vec4 baseColorFactor{1.0f};   // Linear RGBA; albedo for dielectrics, reflectance for metals.
        float metallicFactor = 1.0f;       // 0 = dielectric, 1 = metal. Scales the MR texture's blue channel.
        float roughnessFactor = 1.0f;      // 0 = mirror, 1 = fully rough. Scales the MR texture's green channel.
        float normalScale = 1.0f;          // Strength of the normal texture's XY perturbation.
        float occlusionStrength = 1.0f;    // How much of the occlusion texture's red channel is applied.
        glm::vec3 emissiveFactor{0.0f};    // Linear RGB, added unlit. Scales the emissive texture.
    };

    // The texture slots of a material. A null slot means "unset" and is bound to a neutral
    // default by MaterialSystem (white for most, a flat (0, 0, 1) normal for `normal`), so every
    // material samples every slot and the shader has a single path.
    //
    // Channel conventions follow glTF: metallicRoughness has roughness in G and metallic in B,
    // occlusion is in R, normal is a tangent-space normal in RGB. baseColor and emissive hold
    // sRGB-encoded color (TextureSettings::srgb = true); the others hold linear data (srgb = false).
    struct MaterialTextures {
        std::shared_ptr<Texture> baseColor;
        std::shared_ptr<Texture> metallicRoughness;
        std::shared_ptr<Texture> normal;
        std::shared_ptr<Texture> occlusion;
        std::shared_ptr<Texture> emissive;

        // Descriptor bindings in the material set, in slot order; binding 0 is the uniform buffer.
        static constexpr std::uint32_t kSlotCount = 5;
        static constexpr std::uint32_t kFirstTextureBinding = 1;
    };

    // A surface description ready to be bound for drawing: a uniform buffer holding the
    // MaterialParameters and the descriptor set (set 1) that binds it together with the five
    // textures. Immutable once created; to change a surface, create another material (see
    // MaterialSystem::CreateMaterial) and assign it with Mesh::SetMaterial. Materials are shared
    // between sub-meshes and meshes via std::shared_ptr and keep their textures alive.
    class Material final {
    public:
        // Every texture slot must be set (MaterialSystem fills unset slots with its defaults).
        // The allocator, device and descriptor allocator must outlive the material.
        Material(const VulkanAllocator& allocator, const VulkanDevice& device,
                 VulkanDescriptorAllocator& descriptorAllocator, const MaterialParameters& parameters,
                 MaterialTextures textures);
        ~Material();

        Material(const Material&) = delete;
        Material& operator=(const Material&) = delete;

        Material(Material&&) = delete;
        Material& operator=(Material&&) = delete;

        // The set to bind at the pipeline's material set index (1; see VulkanDescriptorAllocator).
        [[nodiscard]]
        VkDescriptorSet GetDescriptorSet() const noexcept;

        [[nodiscard]]
        const MaterialParameters& GetParameters() const noexcept;

        [[nodiscard]]
        const MaterialTextures& GetTextures() const noexcept;

    private:
        VkDevice m_Device = VK_NULL_HANDLE;                          // Non-owning.
        VulkanDescriptorAllocator* m_DescriptorAllocator = nullptr;  // Non-owning; the set is returned to it.
        MaterialParameters m_Parameters;
        MaterialTextures m_Textures;
        std::unique_ptr<VulkanBuffer> m_UniformBuffer;
        VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    };

}  // namespace Frost
