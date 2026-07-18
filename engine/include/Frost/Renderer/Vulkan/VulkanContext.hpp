#pragma once

#include <vulkan/vulkan.h>

namespace Frost {

    class Window;

    class VulkanContext final {
    public:
        explicit VulkanContext(const Window& window);
        ~VulkanContext();

        VulkanContext(const VulkanContext&) = delete;
        VulkanContext& operator=(const VulkanContext&) = delete;

        VulkanContext(VulkanContext&&) = delete;
        VulkanContext& operator=(VulkanContext&&) = delete;

        [[nodiscard]]
        VkInstance GetInstance() const noexcept;

        [[nodiscard]]
        VkSurfaceKHR GetSurface() const noexcept;

    private:
        static void EnsureVulkan14Support();

        void CreateInstance();
        void CreateSurface(const Window& window);

        VkInstance m_Instance = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    };

}  // namespace Frost