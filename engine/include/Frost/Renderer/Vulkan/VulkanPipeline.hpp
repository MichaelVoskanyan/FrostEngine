#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace Frost {

    class VulkanDevice;

    // A single graphics pipeline built for dynamic rendering (no VkRenderPass/VkFramebuffer):
    // fixed vertex input layout (see Vertex), a single mat4 push constant (model-view-projection),
    // and one color attachment matching the swapchain's format. Depth testing is not wired up yet
    // (no depth image/attachment exists).
    class VulkanPipeline final {
    public:
        VulkanPipeline(const VulkanDevice& device, VkFormat colorAttachmentFormat,
                      const std::string& vertexShaderPath, const std::string& fragmentShaderPath);
        ~VulkanPipeline();

        VulkanPipeline(const VulkanPipeline&) = delete;
        VulkanPipeline& operator=(const VulkanPipeline&) = delete;

        VulkanPipeline(VulkanPipeline&&) = delete;
        VulkanPipeline& operator=(VulkanPipeline&&) = delete;

        [[nodiscard]]
        VkPipeline GetHandle() const noexcept;

        [[nodiscard]]
        VkPipelineLayout GetLayout() const noexcept;

    private:
        [[nodiscard]]
        static std::vector<char> ReadFile(const std::string& path);

        [[nodiscard]]
        VkShaderModule CreateShaderModule(const VulkanDevice& device, const std::vector<char>& code) const;

        void CreatePipeline(const VulkanDevice& device, VkFormat colorAttachmentFormat,
                            const std::string& vertexShaderPath, const std::string& fragmentShaderPath);

        VkDevice m_Device = VK_NULL_HANDLE;  // Non-owning; used to destroy the pipeline/layout.
        VkPipelineLayout m_Layout = VK_NULL_HANDLE;
        VkPipeline m_Pipeline = VK_NULL_HANDLE;
    };

}  // namespace Frost
