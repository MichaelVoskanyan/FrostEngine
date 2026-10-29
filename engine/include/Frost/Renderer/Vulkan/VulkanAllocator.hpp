#pragma once

// Forward-declared to match VMA's own typedef (VK_DEFINE_HANDLE(VmaAllocator)) without pulling
// vk_mem_alloc.h into a public engine header.
struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

namespace Frost {

    class VulkanContext;
    class VulkanPhysicalDevice;
    class VulkanDevice;

    // Owns the single VmaAllocator used for every GPU buffer (and, later, image) allocation made
    // through VulkanBuffer.
    class VulkanAllocator final {
    public:
        VulkanAllocator(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                        const VulkanDevice& device);
        ~VulkanAllocator();

        VulkanAllocator(const VulkanAllocator&) = delete;
        VulkanAllocator& operator=(const VulkanAllocator&) = delete;

        VulkanAllocator(VulkanAllocator&&) = delete;
        VulkanAllocator& operator=(VulkanAllocator&&) = delete;

        [[nodiscard]]
        VmaAllocator GetHandle() const noexcept;

    private:
        VmaAllocator m_Allocator = nullptr;
    };

}  // namespace Frost
