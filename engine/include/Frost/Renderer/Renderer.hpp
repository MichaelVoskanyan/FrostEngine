#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace Frost {

    class Window;
    class VulkanContext;
    class VulkanPhysicalDevice;
    class VulkanDevice;
    class VulkanAllocator;
    class VulkanSwapchain;
    class VulkanCommandPool;
    class VulkanPipeline;
    class Mesh;

    // Top-level render system owned by Engine. Owns the full Vulkan bring-up chain
    // (instance/surface -> physical device -> logical device -> allocator -> swapchain) plus the
    // frame loop itself: a command pool, per-frame sync objects, a dynamic-rendering pipeline, and
    // (temporarily) a single hardcoded test mesh so the chain can be verified end to end before a
    // model/asset system exists to feed it real geometry.
    //
    // Known gaps, deferred to follow-up passes: no swapchain recreation on resize/minimize, no
    // depth buffer, no scene/entity abstraction (Tick() always draws the one test mesh with an
    // identity MVP).
    class Renderer final {
    public:
        explicit Renderer(const Window& window);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        Renderer(Renderer&&) = delete;
        Renderer& operator=(Renderer&&) = delete;

        void Tick();

    private:
        static constexpr std::uint32_t kFramesInFlight = 2;

        struct FrameSync {
            VkSemaphore imageAvailable = VK_NULL_HANDLE;
            VkSemaphore renderFinished = VK_NULL_HANDLE;
            VkFence inFlight = VK_NULL_HANDLE;
        };

        [[nodiscard]]
        static std::unique_ptr<Mesh> CreateTestMesh(const VulkanAllocator& allocator, const VulkanDevice& device,
                                                    const VulkanCommandPool& commandPool);

        void CreateSyncObjects();
        void DestroySyncObjects() noexcept;

        void RecordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) const;

        static void TransitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                                    VkImageLayout newLayout);

        // Declared in dependency order. Destruction occurs in reverse order: the swapchain is
        // destroyed before the logical device, the logical device before the context (which owns
        // the surface the swapchain/device were built against). VulkanPhysicalDevice owns no
        // Vulkan object and has no ordering constraint of its own.
        std::unique_ptr<VulkanContext> m_Context;
        std::unique_ptr<VulkanPhysicalDevice> m_PhysicalDevice;
        std::unique_ptr<VulkanDevice> m_Device;
        std::unique_ptr<VulkanAllocator> m_Allocator;
        std::unique_ptr<VulkanSwapchain> m_Swapchain;
        std::unique_ptr<VulkanCommandPool> m_CommandPool;
        std::unique_ptr<VulkanPipeline> m_Pipeline;
        std::unique_ptr<Mesh> m_TestMesh;

        std::vector<VkCommandBuffer> m_CommandBuffers;

        // Semaphores/fences are plain Vulkan handles (not RAII types), so they are explicitly
        // destroyed by DestroySyncObjects() in ~Renderer(), before the unique_ptr members above
        // tear down the device/context those handles were created against.
        std::vector<FrameSync> m_FrameSync;
        std::uint32_t m_CurrentFrame = 0;
    };

}  // namespace Frost
