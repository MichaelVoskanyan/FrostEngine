#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>

#include <Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp>

#include <cstdint>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << '.';

        return std::runtime_error(message.str());
    }

}  // namespace

namespace Frost {

    VulkanDevice::VulkanDevice(const VulkanPhysicalDevice& physicalDevice) { CreateLogicalDevice(physicalDevice); }

    VulkanDevice::~VulkanDevice() {
        if (m_Device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_Device, nullptr);
            m_Device = VK_NULL_HANDLE;
        }
    }

    VkDevice VulkanDevice::GetHandle() const noexcept { return m_Device; }

    VkQueue VulkanDevice::GetGraphicsQueue() const noexcept { return m_GraphicsQueue; }

    VkQueue VulkanDevice::GetPresentQueue() const noexcept { return m_PresentQueue; }

    void VulkanDevice::CreateLogicalDevice(const VulkanPhysicalDevice& physicalDevice) {
        const QueueFamilyIndices& indices = physicalDevice.GetQueueFamilyIndices();

        // A set, since the graphics and present queue families are often the same index and
        // Vulkan requires each unique family to appear at most once in pQueueCreateInfos.
        const std::set<std::uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
                                                              indices.presentFamily.value()};

        const float queuePriority = 1.0f;

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        queueCreateInfos.reserve(uniqueQueueFamilies.size());

        for (const std::uint32_t queueFamily : uniqueQueueFamilies) {
            queueCreateInfos.push_back(VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                                .pNext = nullptr,
                                                                .flags = 0,
                                                                .queueFamilyIndex = queueFamily,
                                                                .queueCount = 1,
                                                                .pQueuePriorities = &queuePriority});
        }

        // dynamicRendering and synchronization2 are core Vulkan 1.3 structs/functions but are not
        // enabled by default; VulkanPhysicalDevice::CheckFeatureSupport already verified the chosen
        // device supports both. They are requested here via the VkPhysicalDeviceFeatures2 pNext
        // chain rather than the legacy pEnabledFeatures field, which the two are mutually exclusive
        // with (pEnabledFeatures must be null when pNext carries a VkPhysicalDeviceFeatures2).
        VkPhysicalDeviceVulkan13Features vulkan13Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                .pNext = nullptr,
                .synchronization2 = VK_TRUE,
                .dynamicRendering = VK_TRUE,
        };

        const VkPhysicalDeviceFeatures2 features2{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &vulkan13Features,
        };

        const std::vector<const char*>& deviceExtensions = VulkanPhysicalDevice::RequiredExtensions();

        const VkDeviceCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .pNext = &features2,
                .flags = 0,
                .queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size()),
                .pQueueCreateInfos = queueCreateInfos.data(),
                .enabledLayerCount = 0,
                .ppEnabledLayerNames = nullptr,
                .enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size()),
                .ppEnabledExtensionNames = deviceExtensions.data(),
                .pEnabledFeatures = nullptr};

        const VkResult result = vkCreateDevice(physicalDevice.GetHandle(), &createInfo, nullptr, &m_Device);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateDevice", result); }

        vkGetDeviceQueue(m_Device, indices.graphicsFamily.value(), 0, &m_GraphicsQueue);
        vkGetDeviceQueue(m_Device, indices.presentFamily.value(), 0, &m_PresentQueue);
    }

}  // namespace Frost
