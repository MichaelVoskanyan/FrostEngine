#include <Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp>

#include <Frost/Renderer/Vulkan/VulkanContext.hpp>

#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    const std::vector<const char*> kRequiredDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

}  // namespace

namespace Frost {

    bool QueueFamilyIndices::IsComplete() const noexcept {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }

    VulkanPhysicalDevice::VulkanPhysicalDevice(const VulkanContext& context) { SelectPhysicalDevice(context); }

    VkPhysicalDevice VulkanPhysicalDevice::GetHandle() const noexcept { return m_PhysicalDevice; }

    const QueueFamilyIndices& VulkanPhysicalDevice::GetQueueFamilyIndices() const noexcept {
        return m_QueueFamilyIndices;
    }

    const VkPhysicalDeviceProperties& VulkanPhysicalDevice::GetProperties() const noexcept { return m_Properties; }

    const std::vector<const char*>& VulkanPhysicalDevice::RequiredExtensions() { return kRequiredDeviceExtensions; }

    const char* VulkanPhysicalDevice::SwapchainMaintenance1Extension() const noexcept {
        return m_SwapchainMaintenance1Extension;
    }

    SwapchainSupportDetails VulkanPhysicalDevice::QuerySwapchainSupport(const VkSurfaceKHR surface) const {
        SwapchainSupportDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, surface, &details.capabilities);

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, surface, &formatCount, nullptr);

        if (formatCount != 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, surface, &formatCount, details.formats.data());
        }

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, surface, &presentModeCount, nullptr);

        if (presentModeCount != 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, surface, &presentModeCount,
                                                       details.presentModes.data());
        }

        return details;
    }

    void VulkanPhysicalDevice::SelectPhysicalDevice(const VulkanContext& context) {
        const std::vector<VkPhysicalDevice> devices = EnumeratePhysicalDevices(context.GetInstance());

        for (const VkPhysicalDevice device : devices) {
            if (!IsDeviceSuitable(device, context.GetSurface())) { continue; }

            m_PhysicalDevice = device;
            m_QueueFamilyIndices = FindQueueFamilies(device, context.GetSurface());
            vkGetPhysicalDeviceProperties(device, &m_Properties);
            m_SwapchainMaintenance1Extension = DetectSwapchainMaintenance1(device, context);

            return;
        }

        throw std::runtime_error(
                "Failed to find a GPU with the Vulkan support Frost requires (graphics + present queues, "
                "VK_KHR_swapchain, and a usable surface format/present mode).");
    }

    std::vector<VkPhysicalDevice> VulkanPhysicalDevice::EnumeratePhysicalDevices(const VkInstance instance) {
        std::uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

        if (deviceCount == 0) { throw std::runtime_error("Failed to find a GPU with Vulkan support."); }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        return devices;
    }

    bool VulkanPhysicalDevice::IsDeviceSuitable(const VkPhysicalDevice device, const VkSurfaceKHR surface) {
        if (!FindQueueFamilies(device, surface).IsComplete()) { return false; }

        if (!CheckExtensionSupport(device)) { return false; }

        if (!CheckFeatureSupport(device)) { return false; }

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

        return formatCount != 0 && presentModeCount != 0;
    }

    QueueFamilyIndices VulkanPhysicalDevice::FindQueueFamilies(const VkPhysicalDevice device,
                                                                const VkSurfaceKHR surface) {
        QueueFamilyIndices indices;

        std::uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
            if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) { indices.graphicsFamily = i; }

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

            if (presentSupport == VK_TRUE) { indices.presentFamily = i; }

            if (indices.IsComplete()) { break; }
        }

        return indices;
    }

    bool VulkanPhysicalDevice::CheckExtensionSupport(const VkPhysicalDevice device) {
        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());

        for (const VkExtensionProperties& extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    const char* VulkanPhysicalDevice::DetectSwapchainMaintenance1(const VkPhysicalDevice device,
                                                                  const VulkanContext& context) {
        // The device extension is only usable when the instance-level surface_maintenance1 half
        // was enabled too.
        if (!context.HasSurfaceMaintenance1()) { return nullptr; }

        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        const char* extensionName = nullptr;
        for (const VkExtensionProperties& extension : availableExtensions) {
            if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0) {
                extensionName = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
                break;  // Prefer the promoted name when both are present.
            }
            if (std::strcmp(extension.extensionName, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0) {
                extensionName = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
            }
        }

        if (extensionName == nullptr) { return nullptr; }

        // The EXT feature struct is a header alias of the KHR one (identical sType/layout), so it
        // serves for either extension name.
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
                .pNext = nullptr,
                .swapchainMaintenance1 = VK_FALSE,
        };

        VkPhysicalDeviceFeatures2 features2{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &maintenance1Features,
                .features = {},
        };

        vkGetPhysicalDeviceFeatures2(device, &features2);

        return maintenance1Features.swapchainMaintenance1 == VK_TRUE ? extensionName : nullptr;
    }

    bool VulkanPhysicalDevice::CheckFeatureSupport(const VkPhysicalDevice device) {
        VkPhysicalDeviceVulkan13Features vulkan13Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                .pNext = nullptr,
        };

        VkPhysicalDeviceFeatures2 features2{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &vulkan13Features,
        };

        vkGetPhysicalDeviceFeatures2(device, &features2);

        return vulkan13Features.dynamicRendering == VK_TRUE && vulkan13Features.synchronization2 == VK_TRUE;
    }

}  // namespace Frost
