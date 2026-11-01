#include <Frost/Renderer/Vulkan/VulkanContext.hpp>

#include <Frost/Platform/Window.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

#ifdef NDEBUG
    constexpr bool kEnableValidationLayers = false;
#else
    constexpr bool kEnableValidationLayers = true;
#endif

    constexpr std::array<const char*, 1> kValidationLayers = {"VK_LAYER_KHRONOS_validation"};

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << '.';

        return std::runtime_error(message.str());
    }

    // vkCreateDebugUtilsMessengerEXT/vkDestroyDebugUtilsMessengerEXT are extension functions and
    // are not loaded by the Vulkan loader automatically; their addresses must be resolved manually.
    VkResult CreateDebugUtilsMessengerEXT(const VkInstance instance,
                                          const VkDebugUtilsMessengerCreateInfoEXT* const createInfo,
                                          const VkAllocationCallbacks* const allocator,
                                          VkDebugUtilsMessengerEXT* const messenger) {
        const auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));

        return func != nullptr ? func(instance, createInfo, allocator, messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    void DestroyDebugUtilsMessengerEXT(const VkInstance instance, const VkDebugUtilsMessengerEXT messenger,
                                       const VkAllocationCallbacks* const allocator) {
        const auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

        if (func != nullptr) { func(instance, messenger, allocator); }
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* const callbackData,
                                                 void* /*userData*/) {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            std::cerr << "[Vulkan] " << callbackData->pMessage << '\n';
        }

        return VK_FALSE;
    }

}  // namespace

namespace Frost {

    VulkanContext::VulkanContext(const Window& window) {
        if (glfwVulkanSupported() != GLFW_TRUE) {
            throw std::runtime_error("GLFW could not find a supported Vulkan loader");
        }

        EnsureVulkan14Support();
        CreateInstance();

        try {
            SetupDebugMessenger();
            CreateSurface(window);
        }
        catch (...) {
            DestroyDebugMessenger();

            vkDestroyInstance(m_Instance, nullptr);
            m_Instance = VK_NULL_HANDLE;

            throw;
        }
    }

    VulkanContext::~VulkanContext() {
        if (m_Surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
            m_Surface = VK_NULL_HANDLE;
        }

        DestroyDebugMessenger();

        if (m_Instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_Instance, nullptr);
            m_Instance = VK_NULL_HANDLE;
        }
    }

    VkInstance VulkanContext::GetInstance() const noexcept { return m_Instance; }

    VkSurfaceKHR VulkanContext::GetSurface() const noexcept { return m_Surface; }

    bool VulkanContext::HasSurfaceMaintenance1() const noexcept { return m_SurfaceMaintenance1; }

    void VulkanContext::EnsureVulkan14Support() {
        std::uint32_t supportedVersion = VK_API_VERSION_1_0;

        const VkResult result = vkEnumerateInstanceVersion(&supportedVersion);

        if (result != VK_SUCCESS) { throw VulkanError("vkEnumerateInstanceVersion", result); }

        if (supportedVersion < VK_API_VERSION_1_4) {
            std::ostringstream message;

            message << "Vulkan 1.4 is required, but the loader supports Vulkan "
                    << VK_API_VERSION_MAJOR(supportedVersion) << '.' << VK_API_VERSION_MINOR(supportedVersion) << '.'
                    << VK_API_VERSION_PATCH(supportedVersion) << '.';
            throw std::runtime_error(message.str());
        }
    }

    bool VulkanContext::CheckValidationLayerSupport() {
        std::uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        return std::all_of(kValidationLayers.begin(), kValidationLayers.end(), [&availableLayers](const char* const layerName) {
            return std::any_of(availableLayers.begin(), availableLayers.end(),
                                [layerName](const VkLayerProperties& layer) {
                                    return std::strcmp(layerName, layer.layerName) == 0;
                                });
        });
    }

    VkDebugUtilsMessengerCreateInfoEXT VulkanContext::DebugMessengerCreateInfo() noexcept {
        return VkDebugUtilsMessengerCreateInfoEXT{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .pNext = nullptr,
                .flags = 0,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = DebugCallback,
                .pUserData = nullptr,
        };
    }

    void VulkanContext::CreateInstance() {
        if (kEnableValidationLayers && !CheckValidationLayerSupport()) {
            throw std::runtime_error(
                    "Validation layers were requested but are not available. Install the Vulkan SDK to obtain "
                    "VK_LAYER_KHRONOS_validation.");
        }

        std::uint32_t glfwExtensionCount = 0;

        const char** const glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
            throw std::runtime_error(
                    "GLFW did not provide the Vulkan instance extensions required for window surface creation.");
        }

        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        if constexpr (kEnableValidationLayers) { extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

        AppendOptionalInstanceExtensions(extensions);

        const VkApplicationInfo appInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                        .pNext = nullptr,
                                        .pApplicationName = "Frost Application",
                                        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                                        .pEngineName = "Frost Engine",
                                        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                                        .apiVersion = VK_API_VERSION_1_4};

        // Kept alive through the vkCreateInstance call below so it can be chained via pNext; this
        // lets validation catch errors in vkCreateInstance/vkDestroyInstance themselves, which run
        // outside the lifetime of the messenger created by SetupDebugMessenger().
        const VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = DebugMessengerCreateInfo();

        const VkInstanceCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                .pNext = kEnableValidationLayers ? &debugCreateInfo : nullptr,
                .flags = 0,
                .pApplicationInfo = &appInfo,
                .enabledLayerCount = kEnableValidationLayers ? static_cast<std::uint32_t>(kValidationLayers.size()) : 0,
                .ppEnabledLayerNames = kEnableValidationLayers ? kValidationLayers.data() : nullptr,
                .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
                .ppEnabledExtensionNames = extensions.data()};

        const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_Instance);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateInstance", result); }
    }

    void VulkanContext::AppendOptionalInstanceExtensions(std::vector<const char*>& extensions) {
        std::uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);

        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

        const auto has = [&available](const char* const name) {
            return std::any_of(available.begin(), available.end(), [name](const VkExtensionProperties& extension) {
                return std::strcmp(name, extension.extensionName) == 0;
            });
        };

        // surface_maintenance1 (the KHR promotion or the original EXT; same behavior) requires
        // get_surface_capabilities2. Both are needed for the device-level swapchain_maintenance1
        // extension that gives the renderer presentation fences.
        if (!has(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) { return; }

        const char* surfaceMaintenance1 = nullptr;
        if (has(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) { surfaceMaintenance1 = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME; }
        else if (has(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) { surfaceMaintenance1 = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME; }

        if (surfaceMaintenance1 == nullptr) { return; }

        extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        extensions.push_back(surfaceMaintenance1);
        m_SurfaceMaintenance1 = true;
    }

    void VulkanContext::SetupDebugMessenger() {
        if constexpr (!kEnableValidationLayers) { return; }

        const VkDebugUtilsMessengerCreateInfoEXT createInfo = DebugMessengerCreateInfo();

        const VkResult result = CreateDebugUtilsMessengerEXT(m_Instance, &createInfo, nullptr, &m_DebugMessenger);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateDebugUtilsMessengerEXT", result); }
    }

    void VulkanContext::DestroyDebugMessenger() noexcept {
        if (m_DebugMessenger != VK_NULL_HANDLE) {
            DestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
            m_DebugMessenger = VK_NULL_HANDLE;
        }
    }

    void VulkanContext::CreateSurface(const Window& window) {
        const VkResult result = glfwCreateWindowSurface(m_Instance, window.GetNativeHandle(), nullptr, &m_Surface);

        if (result != VK_SUCCESS) { throw VulkanError("glfwCreateWindowSurface", result); }
    }

}  // namespace Frost
