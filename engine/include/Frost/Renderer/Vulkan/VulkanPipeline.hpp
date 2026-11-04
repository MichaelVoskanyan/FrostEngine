#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace Frost {

    class VulkanDevice;

    // A single graphics pipeline built for dynamic rendering (no VkRenderPass/VkFramebuffer):
    // fixed vertex input layout (see Vertex), per-draw push constants (MeshPushConstants, pushed by
    // Mesh::Draw), two descriptor sets (set 0: per-frame uniforms, bound once per frame by
    // Renderer; set 1: the material, bound per sub-mesh by Mesh::Draw; both layouts owned by
    // VulkanDescriptorAllocator), one color attachment matching the swapchain's format, and a depth
    // attachment. Depth uses the reversed-Z convention (near plane at 1, far plane at 0, compare
    // GREATER_OR_EQUAL, cleared to 0), which spreads floating-point depth precision far more
    // evenly over distance than the classic mapping; Camera's projection produces it. Passing
    // VK_FORMAT_UNDEFINED as the depth format builds a pipeline without depth testing.
    class VulkanPipeline final {
    public:
        VulkanPipeline(const VulkanDevice& device, VkFormat colorAttachmentFormat, VkFormat depthAttachmentFormat,
                       VkDescriptorSetLayout frameSetLayout, VkDescriptorSetLayout materialSetLayout,
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

        // The color attachment format this pipeline was compiled against. Viewport/scissor are
        // dynamic, so a swapchain resize alone never invalidates the pipeline, but a format change
        // (possible in principle on swapchain recreation) does.
        [[nodiscard]]
        VkFormat GetColorAttachmentFormat() const noexcept;

    private:
        [[nodiscard]]
        static std::vector<char> ReadFile(const std::string& path);

        [[nodiscard]]
        VkShaderModule CreateShaderModule(const VulkanDevice& device, const std::vector<char>& code) const;

        void CreatePipeline(const VulkanDevice& device, VkFormat colorAttachmentFormat, VkFormat depthAttachmentFormat,
                            VkDescriptorSetLayout frameSetLayout, VkDescriptorSetLayout materialSetLayout,
                            const std::string& vertexShaderPath, const std::string& fragmentShaderPath);

        VkDevice m_Device = VK_NULL_HANDLE;  // Non-owning; used to destroy the pipeline/layout.
        VkFormat m_ColorAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkPipelineLayout m_Layout = VK_NULL_HANDLE;
        VkPipeline m_Pipeline = VK_NULL_HANDLE;
    };

}  // namespace Frost
