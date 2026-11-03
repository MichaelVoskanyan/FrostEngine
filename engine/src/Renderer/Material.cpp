#include <Frost/Renderer/Material.hpp>

#include <Frost/Renderer/ShaderData.hpp>
#include <Frost/Renderer/Vulkan/VulkanBuffer.hpp>
#include <Frost/Renderer/Vulkan/VulkanDescriptorAllocator.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>

#include <stdexcept>
#include <utility>

namespace Frost {

    Material::Material(const VulkanAllocator& allocator, const VulkanDevice& device,
                       VulkanDescriptorAllocator& descriptorAllocator, const MaterialParameters& parameters,
                       MaterialTextures textures)
        : m_Device(device.GetHandle()),
          m_DescriptorAllocator(&descriptorAllocator),
          m_Parameters(parameters),
          m_Textures(std::move(textures)) {
        const std::array<const Texture*, MaterialTextures::kSlotCount> slots = {
                m_Textures.baseColor.get(), m_Textures.metallicRoughness.get(), m_Textures.normal.get(),
                m_Textures.occlusion.get(), m_Textures.emissive.get(),
        };

        for (const Texture* const texture : slots) {
            if (texture == nullptr) {
                throw std::invalid_argument("Material requires every texture slot to be set (see MaterialSystem).");
            }
        }

        // The parameters never change after construction, so a single host-visible write is all
        // the buffer ever sees; no staging copy needed for 48 bytes.
        const MaterialUniforms uniforms{
                .baseColorFactor = parameters.baseColorFactor,
                .emissiveFactor = glm::vec4(parameters.emissiveFactor, 0.0f),
                .metallicFactor = parameters.metallicFactor,
                .roughnessFactor = parameters.roughnessFactor,
                .normalScale = parameters.normalScale,
                .occlusionStrength = parameters.occlusionStrength,
        };

        m_UniformBuffer = std::make_unique<VulkanBuffer>(allocator, sizeof(MaterialUniforms),
                                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        m_UniformBuffer->CopyData(&uniforms, sizeof(uniforms));

        m_DescriptorSet = m_DescriptorAllocator->Allocate(m_DescriptorAllocator->GetMaterialSetLayout());

        const VkDescriptorBufferInfo bufferInfo{
                .buffer = m_UniformBuffer->GetHandle(),
                .offset = 0,
                .range = sizeof(MaterialUniforms),
        };

        std::array<VkDescriptorImageInfo, MaterialTextures::kSlotCount> imageInfos{};
        std::array<VkWriteDescriptorSet, MaterialTextures::kSlotCount + 1> writes{};

        writes[0] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = m_DescriptorSet,
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &bufferInfo,
                .pTexelBufferView = nullptr,
        };

        for (std::uint32_t slot = 0; slot < MaterialTextures::kSlotCount; ++slot) {
            imageInfos[slot] = VkDescriptorImageInfo{
                    .sampler = slots[slot]->GetSampler(),
                    .imageView = slots[slot]->GetImageView(),
                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };

            writes[slot + 1] = VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = m_DescriptorSet,
                    .dstBinding = MaterialTextures::kFirstTextureBinding + slot,
                    .dstArrayElement = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &imageInfos[slot],
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr,
            };
        }

        vkUpdateDescriptorSets(m_Device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    Material::~Material() {
        if (m_DescriptorSet != VK_NULL_HANDLE) {
            m_DescriptorAllocator->Free(m_DescriptorSet);
            m_DescriptorSet = VK_NULL_HANDLE;
        }
    }

    VkDescriptorSet Material::GetDescriptorSet() const noexcept { return m_DescriptorSet; }

    const MaterialParameters& Material::GetParameters() const noexcept { return m_Parameters; }

    const MaterialTextures& Material::GetTextures() const noexcept { return m_Textures; }

}  // namespace Frost
