#include <Frost/Renderer/Vulkan/VulkanDescriptorAllocator.hpp>

#include <Frost/Renderer/Material.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>

#include <array>
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

    VulkanDescriptorAllocator::VulkanDescriptorAllocator(const VulkanDevice& device) : m_Device(device.GetHandle()) {
        // The fragment stage reads the view matrix too (see basic.frag).
        constexpr VkDescriptorSetLayoutBinding frameBinding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = nullptr,
        };

        // Binding 0: MaterialUniforms; bindings 1...: the texture slots, in MaterialTextures order.
        std::array<VkDescriptorSetLayoutBinding, 1 + MaterialTextures::kSlotCount> materialBindings{};
        materialBindings[0] = VkDescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = nullptr,
        };
        for (std::uint32_t slot = 0; slot < MaterialTextures::kSlotCount; ++slot) {
            materialBindings[slot + 1] = VkDescriptorSetLayoutBinding{
                    .binding = MaterialTextures::kFirstTextureBinding + slot,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .pImmutableSamplers = nullptr,
            };
        }

        m_FrameSetLayout = CreateLayout(&frameBinding, 1);

        try {
            m_MaterialSetLayout =
                    CreateLayout(materialBindings.data(), static_cast<std::uint32_t>(materialBindings.size()));
        }
        catch (...) {
            vkDestroyDescriptorSetLayout(m_Device, m_FrameSetLayout, nullptr);
            m_FrameSetLayout = VK_NULL_HANDLE;
            throw;
        }
    }

    VulkanDescriptorAllocator::~VulkanDescriptorAllocator() {
        // Destroying a pool frees every set still allocated from it, so outstanding sets do not
        // leak.
        for (const VkDescriptorPool pool : m_Pools) { vkDestroyDescriptorPool(m_Device, pool, nullptr); }
        m_Pools.clear();
        m_SetPools.clear();

        for (VkDescriptorSetLayout* const layout : {&m_MaterialSetLayout, &m_FrameSetLayout}) {
            if (*layout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(m_Device, *layout, nullptr);
                *layout = VK_NULL_HANDLE;
            }
        }
    }

    VkDescriptorSetLayout VulkanDescriptorAllocator::GetFrameSetLayout() const noexcept { return m_FrameSetLayout; }

    VkDescriptorSetLayout VulkanDescriptorAllocator::GetMaterialSetLayout() const noexcept {
        return m_MaterialSetLayout;
    }

    VkDescriptorSetLayout VulkanDescriptorAllocator::CreateLayout(const VkDescriptorSetLayoutBinding* const bindings,
                                                                  const std::uint32_t bindingCount) const {
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .bindingCount = bindingCount,
                .pBindings = bindings,
        };

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        const VkResult result = vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &layout);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateDescriptorSetLayout", result); }

        return layout;
    }

    VkDescriptorPool VulkanDescriptorAllocator::CreatePool() {
        // Sized so that kSetsPerPool sets of the larger (material) layout fit.
        constexpr std::array<VkDescriptorPoolSize, 2> poolSizes = {
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = kSetsPerPool},
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .descriptorCount = kSetsPerPool * MaterialTextures::kSlotCount},
        };

        const VkDescriptorPoolCreateInfo poolInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                .maxSets = kSetsPerPool,
                .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
                .pPoolSizes = poolSizes.data(),
        };

        VkDescriptorPool pool = VK_NULL_HANDLE;
        const VkResult result = vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &pool);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateDescriptorPool", result); }

        m_Pools.push_back(pool);
        return pool;
    }

    VkDescriptorSet VulkanDescriptorAllocator::Allocate(const VkDescriptorSetLayout layout) {
        VkDescriptorSetAllocateInfo allocInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = nullptr,
                .descriptorPool = VK_NULL_HANDLE,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
        };

        VkDescriptorSet set = VK_NULL_HANDLE;

        // Try the most recent pool first; it is the one most likely to have room. Exhaustion
        // (OUT_OF_POOL_MEMORY, or FRAGMENTED_POOL after frees) is the signal to add a pool.
        if (!m_Pools.empty()) {
            allocInfo.descriptorPool = m_Pools.back();
            const VkResult result = vkAllocateDescriptorSets(m_Device, &allocInfo, &set);

            if (result == VK_SUCCESS) {
                m_SetPools.emplace(set, allocInfo.descriptorPool);
                return set;
            }
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
                throw VulkanError("vkAllocateDescriptorSets", result);
            }
        }

        allocInfo.descriptorPool = CreatePool();
        const VkResult result = vkAllocateDescriptorSets(m_Device, &allocInfo, &set);
        if (result != VK_SUCCESS) { throw VulkanError("vkAllocateDescriptorSets", result); }

        m_SetPools.emplace(set, allocInfo.descriptorPool);
        return set;
    }

    void VulkanDescriptorAllocator::Free(const VkDescriptorSet set) noexcept {
        const auto it = m_SetPools.find(set);
        if (it == m_SetPools.end()) { return; }  // Not ours (or already freed); nothing to do.

        vkFreeDescriptorSets(m_Device, it->second, 1, &set);
        m_SetPools.erase(it);
    }

}  // namespace Frost
