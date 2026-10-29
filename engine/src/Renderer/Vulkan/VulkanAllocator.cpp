#include <Frost/Renderer/Vulkan/VulkanAllocator.hpp>

#include <Frost/Renderer/Vulkan/VulkanContext.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp>

// VMA is a single-header library; exactly one translation unit in the whole program must define
// VMA_IMPLEMENTATION before including it to generate the implementation. This is that TU.
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

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

    VulkanAllocator::VulkanAllocator(const VulkanContext& context, const VulkanPhysicalDevice& physicalDevice,
                                     const VulkanDevice& device) {
        VmaAllocatorCreateInfo createInfo{};
        createInfo.physicalDevice = physicalDevice.GetHandle();
        createInfo.device = device.GetHandle();
        createInfo.instance = context.GetInstance();
        createInfo.vulkanApiVersion = VK_API_VERSION_1_4;

        const VkResult result = vmaCreateAllocator(&createInfo, &m_Allocator);
        if (result != VK_SUCCESS) { throw VulkanError("vmaCreateAllocator", result); }
    }

    VulkanAllocator::~VulkanAllocator() {
        if (m_Allocator != nullptr) {
            vmaDestroyAllocator(m_Allocator);
            m_Allocator = nullptr;
        }
    }

    VmaAllocator VulkanAllocator::GetHandle() const noexcept { return m_Allocator; }

}  // namespace Frost
