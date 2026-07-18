#include <Frost/Renderer/Vulkan/VulkanContext.hpp>

#include <Frost/Platform/Window.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdint>
#include <sstream>
#include <stdexcept>

namespace {

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << ',';

        return std::runtime_error(message.str());
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
            CreateSurface(window);
        }
        catch (...) {
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

        if (m_Instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_Instance, nullptr);
            m_Instance = VK_NULL_HANDLE;
        }
    }

    VkInstance VulkanContext::GetInstance() const noexcept { return m_Instance; }

    VkSurfaceKHR VulkanContext::GetSurface() const noexcept { return m_Surface; }

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

    void VulkanContext::CreateInstance() {
        std::uint32_t extensionCount = 0;

        const char** const requiredExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);

        if (requiredExtensions == nullptr || extensionCount == 0) {
            throw std::runtime_error(
                    "GLFW did not provide the VUlkan instance extensions required for window surface creation.");
        }

        const VkApplicationInfo appInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                        .pNext = nullptr,
                                        .pApplicationName = "Frost Application",
                                        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                                        .pEngineName = "Frost Engine",
                                        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                                        .apiVersion = VK_API_VERSION_1_4};

        const VkInstanceCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                              .pNext = nullptr,
                                              .flags = 0,
                                              .pApplicationInfo = &appInfo,
                                              .enabledLayerCount = 0,
                                              .ppEnabledLayerNames = nullptr,
                                              .enabledExtensionCount = extensionCount,
                                              .ppEnabledExtensionNames = requiredExtensions};

        const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_Instance);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateInstance", result); }
    }

    void VulkanContext::CreateSurface(const Window& window) {
        const VkResult result = glfwCreateWindowSurface(m_Instance, window.GetNativeHandle(), nullptr, &m_Surface);

        if (result != VK_SUCCESS) { throw VulkanError("glfwCreateWindowSurface", result); }
    }

}  // namespace Frost