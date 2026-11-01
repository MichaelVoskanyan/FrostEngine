#include <Frost/Renderer/Vulkan/VulkanPipeline.hpp>

#include <Frost/Renderer/Vertex.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>

#include <array>
#include <cstdint>
#include <fstream>
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

    VulkanPipeline::VulkanPipeline(const VulkanDevice& device, const VkFormat colorAttachmentFormat,
                                   const std::string& vertexShaderPath, const std::string& fragmentShaderPath)
        : m_Device(device.GetHandle()), m_ColorAttachmentFormat(colorAttachmentFormat) {
        CreatePipeline(device, colorAttachmentFormat, vertexShaderPath, fragmentShaderPath);
    }

    VulkanPipeline::~VulkanPipeline() {
        if (m_Pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
            m_Pipeline = VK_NULL_HANDLE;
        }

        if (m_Layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_Device, m_Layout, nullptr);
            m_Layout = VK_NULL_HANDLE;
        }
    }

    VkPipeline VulkanPipeline::GetHandle() const noexcept { return m_Pipeline; }

    VkPipelineLayout VulkanPipeline::GetLayout() const noexcept { return m_Layout; }

    VkFormat VulkanPipeline::GetColorAttachmentFormat() const noexcept { return m_ColorAttachmentFormat; }

    std::vector<char> VulkanPipeline::ReadFile(const std::string& path) {
        std::ifstream file(path, std::ios::ate | std::ios::binary);

        if (!file.is_open()) {
            throw std::runtime_error("Failed to open shader file: " + path +
                                     " (has it been compiled? see cmake/Shaders.cmake)");
        }

        const std::size_t fileSize = static_cast<std::size_t>(file.tellg());
        std::vector<char> buffer(fileSize);

        file.seekg(0);
        file.read(buffer.data(), static_cast<std::streamsize>(fileSize));

        return buffer;
    }

    VkShaderModule VulkanPipeline::CreateShaderModule(const VulkanDevice& device,
                                                      const std::vector<char>& code) const {
        const VkShaderModuleCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .codeSize = code.size(),
                .pCode = reinterpret_cast<const std::uint32_t*>(code.data()),
        };

        VkShaderModule shaderModule = VK_NULL_HANDLE;

        const VkResult result = vkCreateShaderModule(device.GetHandle(), &createInfo, nullptr, &shaderModule);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateShaderModule", result); }

        return shaderModule;
    }

    void VulkanPipeline::CreatePipeline(const VulkanDevice& device, const VkFormat colorAttachmentFormat,
                                        const std::string& vertexShaderPath, const std::string& fragmentShaderPath) {
        const VkShaderModule vertexModule = CreateShaderModule(device, ReadFile(vertexShaderPath));
        const VkShaderModule fragmentModule = CreateShaderModule(device, ReadFile(fragmentShaderPath));

        const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{
                VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .pNext = nullptr,
                                                .flags = 0,
                                                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                .module = vertexModule,
                                                .pName = "main",
                                                .pSpecializationInfo = nullptr},
                VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .pNext = nullptr,
                                                .flags = 0,
                                                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                .module = fragmentModule,
                                                .pName = "main",
                                                .pSpecializationInfo = nullptr},
        };

        const VkVertexInputBindingDescription bindingDescription = Vertex::GetBindingDescription();
        const std::array<VkVertexInputAttributeDescription, 4> attributeDescriptions =
                Vertex::GetAttributeDescriptions();

        const VkPipelineVertexInputStateCreateInfo vertexInputState{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .vertexBindingDescriptionCount = 1,
                .pVertexBindingDescriptions = &bindingDescription,
                .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributeDescriptions.size()),
                .pVertexAttributeDescriptions = attributeDescriptions.data(),
        };

        constexpr VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                .primitiveRestartEnable = VK_FALSE,
        };

        // Viewport/scissor are supplied dynamically per-frame (Renderer sets them from the current
        // swapchain extent), so no actual viewport/scissor rectangles are provided here.
        constexpr VkPipelineViewportStateCreateInfo viewportState{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .viewportCount = 1,
                .pViewports = nullptr,
                .scissorCount = 1,
                .pScissors = nullptr,
        };

        constexpr VkPipelineRasterizationStateCreateInfo rasterizationState{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .depthClampEnable = VK_FALSE,
                .rasterizerDiscardEnable = VK_FALSE,
                .polygonMode = VK_POLYGON_MODE_FILL,
                .cullMode = VK_CULL_MODE_BACK_BIT,
                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                .depthBiasEnable = VK_FALSE,
                .depthBiasConstantFactor = 0.0f,
                .depthBiasClamp = 0.0f,
                .depthBiasSlopeFactor = 0.0f,
                .lineWidth = 1.0f,
        };

        constexpr VkPipelineMultisampleStateCreateInfo multisampleState{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                .sampleShadingEnable = VK_FALSE,
                .minSampleShading = 1.0f,
                .pSampleMask = nullptr,
                .alphaToCoverageEnable = VK_FALSE,
                .alphaToOneEnable = VK_FALSE,
        };

        // No depth attachment yet, so depth test/write stay disabled; see VulkanPipeline's header
        // comment. A depth image/attachment is a follow-up pass.
        constexpr VkPipelineDepthStencilStateCreateInfo depthStencilState{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .depthTestEnable = VK_FALSE,
                .depthWriteEnable = VK_FALSE,
                .depthCompareOp = VK_COMPARE_OP_ALWAYS,
                .depthBoundsTestEnable = VK_FALSE,
                .stencilTestEnable = VK_FALSE,
                .front = {},
                .back = {},
                .minDepthBounds = 0.0f,
                .maxDepthBounds = 1.0f,
        };

        constexpr VkPipelineColorBlendAttachmentState colorBlendAttachment{
                .blendEnable = VK_FALSE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                  VK_COLOR_COMPONENT_A_BIT,
        };

        const VkPipelineColorBlendStateCreateInfo colorBlendState{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .logicOpEnable = VK_FALSE,
                .logicOp = VK_LOGIC_OP_COPY,
                .attachmentCount = 1,
                .pAttachments = &colorBlendAttachment,
                .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
        };

        constexpr std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                                  VK_DYNAMIC_STATE_SCISSOR};

        const VkPipelineDynamicStateCreateInfo dynamicState{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
                .pDynamicStates = dynamicStates.data(),
        };

        const VkPushConstantRange pushConstantRange{
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                .size = sizeof(float) * 16,  // sizeof(glm::mat4), without needing a glm include here.
        };

        const VkPipelineLayoutCreateInfo layoutInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .setLayoutCount = 0,
                .pSetLayouts = nullptr,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &pushConstantRange,
        };

        VkResult result = vkCreatePipelineLayout(device.GetHandle(), &layoutInfo, nullptr, &m_Layout);
        if (result != VK_SUCCESS) {
            vkDestroyShaderModule(device.GetHandle(), fragmentModule, nullptr);
            vkDestroyShaderModule(device.GetHandle(), vertexModule, nullptr);
            throw VulkanError("vkCreatePipelineLayout", result);
        }

        const VkPipelineRenderingCreateInfo renderingCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .pNext = nullptr,
                .viewMask = 0,
                .colorAttachmentCount = 1,
                .pColorAttachmentFormats = &colorAttachmentFormat,
                .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
                .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };

        const VkGraphicsPipelineCreateInfo pipelineInfo{
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .pNext = &renderingCreateInfo,
                .flags = 0,
                .stageCount = static_cast<std::uint32_t>(shaderStages.size()),
                .pStages = shaderStages.data(),
                .pVertexInputState = &vertexInputState,
                .pInputAssemblyState = &inputAssemblyState,
                .pTessellationState = nullptr,
                .pViewportState = &viewportState,
                .pRasterizationState = &rasterizationState,
                .pMultisampleState = &multisampleState,
                .pDepthStencilState = &depthStencilState,
                .pColorBlendState = &colorBlendState,
                .pDynamicState = &dynamicState,
                .layout = m_Layout,
                .renderPass = VK_NULL_HANDLE,
                .subpass = 0,
                .basePipelineHandle = VK_NULL_HANDLE,
                .basePipelineIndex = -1,
        };

        result = vkCreateGraphicsPipelines(device.GetHandle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                           &m_Pipeline);

        vkDestroyShaderModule(device.GetHandle(), fragmentModule, nullptr);
        vkDestroyShaderModule(device.GetHandle(), vertexModule, nullptr);

        if (result != VK_SUCCESS) { throw VulkanError("vkCreateGraphicsPipelines", result); }
    }

}  // namespace Frost
