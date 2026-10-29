#include <Frost/Renderer/Renderer.hpp>

#include <Frost/Platform/Window.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Renderer/Vertex.hpp>
#include <Frost/Renderer/Vulkan/VulkanAllocator.hpp>
#include <Frost/Renderer/Vulkan/VulkanCommandPool.hpp>
#include <Frost/Renderer/Vulkan/VulkanContext.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanPipeline.hpp>
#include <Frost/Renderer/Vulkan/VulkanSwapchain.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <limits>
#include <memory>
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

    Renderer::Renderer(const Window& window) {
        m_Context = std::make_unique<VulkanContext>(window);
        m_PhysicalDevice = std::make_unique<VulkanPhysicalDevice>(*m_Context);
        m_Device = std::make_unique<VulkanDevice>(*m_PhysicalDevice);
        m_Allocator = std::make_unique<VulkanAllocator>(*m_Context, *m_PhysicalDevice, *m_Device);
        m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Context, *m_PhysicalDevice, *m_Device, window);
        m_CommandPool = std::make_unique<VulkanCommandPool>(*m_PhysicalDevice, *m_Device);

        m_Pipeline = std::make_unique<VulkanPipeline>(*m_Device, m_Swapchain->GetImageFormat(),
                                                      "shaders/basic.vert.spv", "shaders/basic.frag.spv");

        m_CommandBuffers = m_CommandPool->AllocateCommandBuffers(*m_Device, kFramesInFlight);

        CreateSyncObjects();

        m_TestMesh = CreateTestMesh(*m_Allocator, *m_Device, *m_CommandPool);
    }

    Renderer::~Renderer() {
        // Nothing below may be destroyed while the device could still be using it.
        vkDeviceWaitIdle(m_Device->GetHandle());

        DestroySyncObjects();
    }

    std::unique_ptr<Mesh> Renderer::CreateTestMesh(const VulkanAllocator& allocator, const VulkanDevice& device,
                                                   const VulkanCommandPool& commandPool) {
        // Temporary hardcoded geometry to prove the buffer/pipeline chain end to end; real content
        // arrives once model loading exists. A single quad in the XY plane, rendered with an
        // identity MVP (no camera/projection exists yet either), so it appears centered on screen.
        const std::vector<Vertex> vertices = {
                Vertex{.position = {-0.5f, -0.5f, 0.0f},
                      .normal = {0.0f, 0.0f, 1.0f},
                      .uv = {0.0f, 0.0f},
                      .color = {1.0f, 0.0f, 0.0f, 1.0f}},
                Vertex{.position = {0.5f, -0.5f, 0.0f},
                      .normal = {0.0f, 0.0f, 1.0f},
                      .uv = {1.0f, 0.0f},
                      .color = {0.0f, 1.0f, 0.0f, 1.0f}},
                Vertex{.position = {0.5f, 0.5f, 0.0f},
                      .normal = {0.0f, 0.0f, 1.0f},
                      .uv = {1.0f, 1.0f},
                      .color = {0.0f, 0.0f, 1.0f, 1.0f}},
                Vertex{.position = {-0.5f, 0.5f, 0.0f},
                      .normal = {0.0f, 0.0f, 1.0f},
                      .uv = {0.0f, 1.0f},
                      .color = {1.0f, 1.0f, 1.0f, 1.0f}},
        };

        const std::vector<std::uint32_t> indices = {0, 1, 2, 0, 2, 3};

        return std::make_unique<Mesh>(allocator, device, commandPool, vertices, indices);
    }

    void Renderer::CreateSyncObjects() {
        m_FrameSync.resize(kFramesInFlight);

        constexpr VkSemaphoreCreateInfo semaphoreInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
        };

        constexpr VkFenceCreateInfo fenceInfo{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };

        const VkDevice device = m_Device->GetHandle();

        for (FrameSync& sync : m_FrameSync) {
            VkResult result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &sync.imageAvailable);
            if (result != VK_SUCCESS) { throw VulkanError("vkCreateSemaphore", result); }

            result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &sync.renderFinished);
            if (result != VK_SUCCESS) { throw VulkanError("vkCreateSemaphore", result); }

            result = vkCreateFence(device, &fenceInfo, nullptr, &sync.inFlight);
            if (result != VK_SUCCESS) { throw VulkanError("vkCreateFence", result); }
        }
    }

    void Renderer::DestroySyncObjects() noexcept {
        const VkDevice device = m_Device->GetHandle();

        for (FrameSync& sync : m_FrameSync) {
            if (sync.renderFinished != VK_NULL_HANDLE) { vkDestroySemaphore(device, sync.renderFinished, nullptr); }
            if (sync.imageAvailable != VK_NULL_HANDLE) { vkDestroySemaphore(device, sync.imageAvailable, nullptr); }
            if (sync.inFlight != VK_NULL_HANDLE) { vkDestroyFence(device, sync.inFlight, nullptr); }
        }

        m_FrameSync.clear();
    }

    void Renderer::Tick() {
        const VkDevice device = m_Device->GetHandle();
        FrameSync& sync = m_FrameSync[m_CurrentFrame];

        vkWaitForFences(device, 1, &sync.inFlight, VK_TRUE, std::numeric_limits<std::uint64_t>::max());

        std::uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(device, m_Swapchain->GetHandle(),
                                                std::numeric_limits<std::uint64_t>::max(), sync.imageAvailable,
                                                VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Swapchain recreation is a follow-up pass; skip this frame rather than crash on
            // resize/minimize.
            return;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) { throw VulkanError("vkAcquireNextImageKHR", result); }

        vkResetFences(device, 1, &sync.inFlight);

        const VkCommandBuffer commandBuffer = m_CommandBuffers[m_CurrentFrame];
        vkResetCommandBuffer(commandBuffer, 0);
        RecordCommandBuffer(commandBuffer, imageIndex);

        const VkSemaphoreSubmitInfo waitSemaphoreInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = sync.imageAvailable,
                .value = 0,
                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .deviceIndex = 0,
        };

        const VkSemaphoreSubmitInfo signalSemaphoreInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = sync.renderFinished,
                .value = 0,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                .deviceIndex = 0,
        };

        const VkCommandBufferSubmitInfo commandBufferInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = commandBuffer,
                .deviceMask = 0,
        };

        const VkSubmitInfo2 submitInfo{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .pNext = nullptr,
                .flags = 0,
                .waitSemaphoreInfoCount = 1,
                .pWaitSemaphoreInfos = &waitSemaphoreInfo,
                .commandBufferInfoCount = 1,
                .pCommandBufferInfos = &commandBufferInfo,
                .signalSemaphoreInfoCount = 1,
                .pSignalSemaphoreInfos = &signalSemaphoreInfo,
        };

        result = vkQueueSubmit2(m_Device->GetGraphicsQueue(), 1, &submitInfo, sync.inFlight);
        if (result != VK_SUCCESS) { throw VulkanError("vkQueueSubmit2", result); }

        const VkSwapchainKHR swapchain = m_Swapchain->GetHandle();
        const VkPresentInfoKHR presentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = nullptr,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &sync.renderFinished,
                .swapchainCount = 1,
                .pSwapchains = &swapchain,
                .pImageIndices = &imageIndex,
                .pResults = nullptr,
        };

        result = vkQueuePresentKHR(m_Device->GetPresentQueue(), &presentInfo);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
            throw VulkanError("vkQueuePresentKHR", result);
        }

        m_CurrentFrame = (m_CurrentFrame + 1) % kFramesInFlight;
    }

    void Renderer::RecordCommandBuffer(const VkCommandBuffer commandBuffer, const std::uint32_t imageIndex) const {
        constexpr VkCommandBufferBeginInfo beginInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = 0,
                .pInheritanceInfo = nullptr,
        };

        VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) { throw VulkanError("vkBeginCommandBuffer", result); }

        const VkImage image = m_Swapchain->GetImages()[imageIndex];
        const VkImageView imageView = m_Swapchain->GetImageViews()[imageIndex];
        const VkExtent2D extent = m_Swapchain->GetExtent();

        TransitionImage(commandBuffer, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        const VkClearValue clearColor{.color = {.float32 = {0.02f, 0.02f, 0.05f, 1.0f}}};

        const VkRenderingAttachmentInfo colorAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = imageView,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .resolveImageView = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = clearColor,
        };

        const VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext = nullptr,
                .flags = 0,
                .renderArea = {.offset = {0, 0}, .extent = extent},
                .layerCount = 1,
                .viewMask = 0,
                .colorAttachmentCount = 1,
                .pColorAttachments = &colorAttachment,
                .pDepthAttachment = nullptr,
                .pStencilAttachment = nullptr,
        };

        vkCmdBeginRendering(commandBuffer, &renderingInfo);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline->GetHandle());

        // Flip the viewport vertically (negative height, y at the bottom) so +Y is up in
        // model/world space, matching the winding the pipeline's CCW front-face setting assumes.
        // Vulkan's window-space Y otherwise points down, which would invert front-facing triangles.
        const VkViewport viewport{
                .x = 0.0f,
                .y = static_cast<float>(extent.height),
                .width = static_cast<float>(extent.width),
                .height = -static_cast<float>(extent.height),
                .minDepth = 0.0f,
                .maxDepth = 1.0f,
        };
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        const VkRect2D scissor{.offset = {0, 0}, .extent = extent};
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        const glm::mat4 mvp(1.0f);
        vkCmdPushConstants(commandBuffer, m_Pipeline->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                           &mvp);

        m_TestMesh->Draw(commandBuffer);

        vkCmdEndRendering(commandBuffer);

        TransitionImage(commandBuffer, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS) { throw VulkanError("vkEndCommandBuffer", result); }
    }

    void Renderer::TransitionImage(const VkCommandBuffer commandBuffer, const VkImage image,
                                   const VkImageLayout oldLayout, const VkImageLayout newLayout) {
        // A deliberately coarse (ALL_COMMANDS) barrier rather than precisely scoped stage/access
        // masks: correct and simple for a single-mesh bring-up pass, at the cost of some GPU
        // synchronization efficiency. Worth tightening once there is more than one draw per frame.
        const VkImageMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                .oldLayout = oldLayout,
                .newLayout = newLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .baseMipLevel = 0,
                                     .levelCount = 1,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
        };

        const VkDependencyInfo dependencyInfo{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext = nullptr,
                .dependencyFlags = 0,
                .memoryBarrierCount = 0,
                .pMemoryBarriers = nullptr,
                .bufferMemoryBarrierCount = 0,
                .pBufferMemoryBarriers = nullptr,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &barrier,
        };

        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

}  // namespace Frost
