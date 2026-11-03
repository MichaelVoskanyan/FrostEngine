#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Frost {

    class VulkanDevice;

    // Owns the graphics pipeline's descriptor set layouts and the pools their sets are allocated
    // from:
    //   set 0 - per-frame constants: one uniform buffer (FrameUniforms), vertex + fragment stages;
    //   set 1 - per-material: one uniform buffer (MaterialUniforms) at binding 0 and the material's
    //           textures (MaterialTextures::kSlotCount combined image samplers) at bindings 1...,
    //           fragment stage.
    // Pools are created on demand, so the number of live sets is not bounded up front; sets are
    // returned individually (the pools allow free), so materials can come and go independently.
    class VulkanDescriptorAllocator final {
    public:
        explicit VulkanDescriptorAllocator(const VulkanDevice& device);
        ~VulkanDescriptorAllocator();

        VulkanDescriptorAllocator(const VulkanDescriptorAllocator&) = delete;
        VulkanDescriptorAllocator& operator=(const VulkanDescriptorAllocator&) = delete;

        VulkanDescriptorAllocator(VulkanDescriptorAllocator&&) = delete;
        VulkanDescriptorAllocator& operator=(VulkanDescriptorAllocator&&) = delete;

        [[nodiscard]]
        VkDescriptorSetLayout GetFrameSetLayout() const noexcept;

        [[nodiscard]]
        VkDescriptorSetLayout GetMaterialSetLayout() const noexcept;

        // Allocates one set of the given layout (one of the two above). The set should be returned
        // with Free() before this allocator is destroyed (destroying the pools releases stragglers).
        [[nodiscard]]
        VkDescriptorSet Allocate(VkDescriptorSetLayout layout);

        void Free(VkDescriptorSet set) noexcept;

    private:
        // Sets per pool (each pool also holds that many descriptors of each type, so a set of
        // either layout fits). Pools are cheap; this only bounds how often a new one is created.
        static constexpr std::uint32_t kSetsPerPool = 64;

        [[nodiscard]]
        VkDescriptorSetLayout CreateLayout(const VkDescriptorSetLayoutBinding* bindings,
                                           std::uint32_t bindingCount) const;

        [[nodiscard]]
        VkDescriptorPool CreatePool();

        VkDevice m_Device = VK_NULL_HANDLE;  // Non-owning.
        VkDescriptorSetLayout m_FrameSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_MaterialSetLayout = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> m_Pools;
        std::unordered_map<VkDescriptorSet, VkDescriptorPool> m_SetPools;  // Which pool each live set came from.
    };

}  // namespace Frost
