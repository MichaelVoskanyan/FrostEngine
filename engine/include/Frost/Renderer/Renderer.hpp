#pragma once

#include <vulkan/vulkan.h>

#include <Frost/Renderer/Vulkan/VulkanSwapchain.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Frost {

    class Window;
    class VulkanContext;
    class VulkanPhysicalDevice;
    class VulkanDevice;
    class VulkanAllocator;
    class VulkanCommandPool;
    class VulkanPipeline;
    class Mesh;

    // Top-level render system owned by Engine. Owns the full Vulkan bring-up chain
    // (instance/surface -> physical device -> logical device -> allocator -> swapchain) plus the
    // frame loop itself: a command pool, per-frame sync objects, a dynamic-rendering pipeline, and
    // (temporarily) a single hardcoded test mesh so the chain can be verified end to end before a
    // model/asset system exists to feed it real geometry.
    //
    // The swapchain is rebuilt whenever the window reports a framebuffer resize or the presentation
    // engine reports the current one out of date/suboptimal; frames are skipped while the window is
    // minimized (zero-area framebuffer). Rebuilding never stalls the GPU: the replaced swapchain,
    // its views, its per-image semaphores (and the pipeline, if its format changed) are parked in
    // m_Retired and destroyed later, once frame-fence bookkeeping proves the GPU is done with them
    // and the presentation engine is done with its presents (see ReleaseRetiredResources).
    //
    // Known gaps, deferred to follow-up passes: no depth buffer, no scene/entity abstraction
    // (Tick() always draws the one test mesh, with only an aspect-ratio-correcting projection in
    // place of a real camera).
    class Renderer final {
    public:
        // The window is held by reference for the renderer's lifetime (Engine guarantees it
        // outlives the renderer); non-const because resize notifications are consumed from it.
        explicit Renderer(Window& window);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        Renderer(Renderer&&) = delete;
        Renderer& operator=(Renderer&&) = delete;

        void Tick();

    private:
        static constexpr std::uint32_t kFramesInFlight = 2;

        // Per frame-in-flight. The render-finished semaphore deliberately does not live here: it is
        // consumed by the presentation engine, whose lifetime is tied to the swapchain image rather
        // than to the frame slot, so it is kept per swapchain image in m_RenderFinished instead.
        struct FrameSync {
            VkSemaphore imageAvailable = VK_NULL_HANDLE;
            VkFence inFlight = VK_NULL_HANDLE;
            // Frame number (1-based, see m_SubmittedFrame) of the submission inFlight guards; 0
            // until the slot's first submit.
            std::uint64_t frameNumber = 0;
        };

        // Everything a swapchain recreation replaces, kept alive until it is provably unused.
        struct RetiredResources {
            VulkanSwapchain::Retired swapchain;
            std::vector<VkSemaphore> renderFinished;   // That swapchain's per-image semaphores.
            std::unique_ptr<VulkanPipeline> pipeline;  // Only if the pipeline had to be rebuilt.
            std::vector<VkFence> presentFences;        // Presentation fences of presents on it (extension path).
            // m_SubmittedFrame at retirement: the last submission that can reference any of this.
            std::uint64_t retireFrame = 0;
            bool retentionWarned = false;  // Diagnostic emitted once if this lingers abnormally long.
        };

        // Backstop only: if this many retired swapchains are pending at once (their presents never
        // completing would be the only way), fall back to a full device idle and flush them all.
        //
        // Sizing: a bundle normally releases once the frame *after* its retirement has completed,
        // which the fence waits observe kFramesInFlight frames later. With a recreation on every
        // frame (continuous resize drag, or a persistently VK_SUBOPTIMAL_KHR surface) that gives a
        // legitimate steady state of kFramesInFlight + 1 pending bundles. The backstop must sit
        // well above that, or an ordinary resize drag would hit the stall it exists to avoid.
        static constexpr std::size_t kMaxRetiredSwapchains = 8;

        // Diagnostic threshold: a retired bundle normally releases within ~kFramesInFlight + 1
        // frames. Still being pending this many submissions later means its presentation fences
        // are not signaling (or the fallback inference never became true) and is worth reporting.
        static constexpr std::uint64_t kRetentionWarningFrames = 120;

        // Diagnostic threshold for the live presentation-fence list. It normally holds about as
        // many fences as there are presents the engine has not finished with (a few). Growing past
        // this means presentation fences are not signaling and the pool is leaking one per frame.
        static constexpr std::size_t kLivePresentFenceWarningCount = 64;

        [[nodiscard]]
        static std::unique_ptr<Mesh> CreateTestMesh(const VulkanAllocator& allocator, const VulkanDevice& device,
                                                    const VulkanCommandPool& commandPool);

        void CreateSyncObjects();
        void DestroySyncObjects() noexcept;

        // Split out of the above because they are also redone on swapchain recreation: the
        // render-finished semaphores are per swapchain image, and the image count may change.
        void CreateRenderFinishedSemaphores();
        void DestroyRenderFinishedSemaphores() noexcept;

        // Rebuilds the swapchain (and whatever depends on it) at the surface's current size without
        // waiting for the GPU: the replaced resources are moved to m_Retired. Clears the window's
        // pending-resize flag. Returns false if the surface currently has zero area (nothing is
        // rebuilt; the caller must skip the frame, since the existing swapchain is known stale).
        [[nodiscard]]
        bool RecreateSwapchain();

        // Destroys every entry of m_Retired whose resources are provably unused: GPU side via
        // frame-fence bookkeeping (m_CompletedFrame), presentation side via presentation fences
        // when available or the documented fallback otherwise. Non-blocking unless `force`, which
        // (at shutdown, after a device idle) waits for presentation fences and releases everything.
        void ReleaseRetiredResources(bool force);

        // Presentation fences (only used when the device has swapchain-maintenance1). A fence is
        // attached to each present and recycled once signaled.
        [[nodiscard]]
        VkFence AcquirePresentFence();
        void RecycleSignaledPresentFences(std::vector<VkFence>& fences);
        void DestroyPresentFences() noexcept;

        void RecordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) const;

        // Records a single image layout transition. The stage/access masks must describe the work
        // that produces the image in oldLayout (src) and the work that consumes it in newLayout
        // (dst) so the transition is correctly ordered against both; see RecordCommandBuffer.
        static void TransitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                                    VkImageLayout newLayout, VkPipelineStageFlags2 srcStageMask,
                                    VkAccessFlags2 srcAccessMask, VkPipelineStageFlags2 dstStageMask,
                                    VkAccessFlags2 dstAccessMask);

        Window& m_Window;  // Non-owning; see the constructor.

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

        // One render-finished semaphore per swapchain image, indexed by the acquired image index.
        // vkQueuePresentKHR gives no way to know when the presentation engine has finished waiting
        // on a semaphore, but re-acquiring the same image does prove it, so keying the semaphore by
        // image (rather than by frame-in-flight) is what makes re-signaling it safe.
        std::vector<VkSemaphore> m_RenderFinished;

        std::uint32_t m_CurrentFrame = 0;

        // Frame-number bookkeeping that replaces vkDeviceWaitIdle for deferred destruction. A fence
        // signal's first synchronization scope includes every command earlier in submission order
        // on that queue (Vulkan spec, "Fence Signaling"), so observing slot N's fence signaled
        // proves every submission numbered <= N has completed.
        std::uint64_t m_SubmittedFrame = 0;  // Number of the most recent vkQueueSubmit2 (1-based).
        std::uint64_t m_CompletedFrame = 0;  // Highest frame number whose fence has been seen signaled.

        std::vector<RetiredResources> m_Retired;

        // Presentation-completion tracking. With swapchain-maintenance1 every present carries a
        // fence (exact, spec-guaranteed). Without it, see ReleaseRetiredResources for the fallback.
        bool m_UsePresentFences = false;
        bool m_PresentQueueIsGraphicsQueue = true;
        std::vector<VkFence> m_FreePresentFences;  // Unsignaled, ready to attach to a present.
        std::vector<VkFence> m_LivePresentFences;  // Attached to presents on the live swapchain, not yet seen signaled.
        bool m_PresentFenceGrowthWarned = false;   // Diagnostic emitted once; see kLivePresentFenceWarningCount.
    };

}  // namespace Frost
