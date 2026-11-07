#pragma once

#include <vulkan/vulkan.h>

#include <Frost/Renderer/Camera.hpp>
#include <Frost/Renderer/Light.hpp>
#include <Frost/Renderer/Vulkan/VulkanSwapchain.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Frost {

    class Window;
    class VulkanContext;
    class VulkanPhysicalDevice;
    class VulkanDevice;
    class VulkanAllocator;
    class VulkanBuffer;
    class VulkanCommandPool;
    class VulkanDescriptorAllocator;
    class VulkanPipeline;
    class VulkanImage;
    class Material;
    class MaterialSystem;
    class Mesh;
    class RenderOverlay;
    struct MeshData;
    struct SkinnedVertex;

    // Top-level render system owned by Engine. Owns the full Vulkan bring-up chain
    // (instance/surface -> physical device -> logical device -> allocator -> swapchain) plus the
    // frame loop itself: a command pool, per-frame sync objects, uniform buffers (view and
    // projection matrices, ambient light; see FrameUniforms) and light buffers (see GpuLight), a
    // descriptor allocator, a depth buffer, a dynamic-rendering pipeline and the material system.
    //
    // What is drawn is decided by the application each frame: every Submit() between two Tick()s
    // is one draw of a mesh (with its own transform and optional material overrides), and Tick()
    // records them all. Skinned meshes are submitted with SubmitSkinned() and their joint palette:
    // the palettes go into the frame's joint buffer (set 0, binding 2), and the meshes are drawn
    // with a second pipeline whose vertex shader (skinned.vert) deforms them by it. Lighting works the same way: every SubmitLight() adds one light to that
    // frame (uploaded to the shaders' light buffer), on top of a flat ambient term
    // (SetAmbientLight). There are no built-in lights: a frame with none submitted is lit by the
    // ambient term alone. Submitted meshes/materials are kept alive by the renderer until the GPU
    // has finished the frame that drew them, so callers may drop their own references (delete an
    // object, unload an asset) at any time.
    //
    // The swapchain is rebuilt whenever the window reports a framebuffer resize or the presentation
    // engine reports the current one out of date/suboptimal; frames are skipped while the window is
    // minimized (zero-area framebuffer). Rebuilding never stalls the GPU: the replaced swapchain,
    // its views, its per-image semaphores, the depth image (and the pipeline, if its format
    // changed) are parked in m_Retired and destroyed later, once frame-fence bookkeeping proves the
    // GPU is done with them and the presentation engine is done with its presents (see
    // ReleaseRetiredResources).
    //
    // The view is taken from the Camera most recently given to SetCamera(); until one is set, a
    // placeholder is used (identity view, fixed aspect-ratio-correcting orthographic projection;
    // see UpdateFrameUniforms). The scene covers the whole swapchain image unless a sub-rectangle
    // is set with SetSceneViewport() (the editor renders the scene into its viewport panel's
    // area). An optional RenderOverlay (SetOverlay) is recorded after the scene each frame, into
    // the same rendering scope; the editor's ImGui UI is drawn that way.
    class Renderer final {
    public:
        // Frames that may be recorded/submitted before the GPU is known to have finished the
        // oldest of them. Anything that writes per-frame GPU resources (see RenderOverlay) must
        // multi-buffer them at least this deep.
        static constexpr std::uint32_t kFramesInFlight = 2;

        // Lights per frame the light buffer holds; SubmitLight() calls past this are ignored (with
        // a one-time warning).
        static constexpr std::uint32_t kMaxLights = 1024;

        // Joint matrices per frame the joint buffer holds (2 MiB): every skinned draw's palette.
        // SubmitSkinned() calls whose palette no longer fits are ignored (with a one-time warning).
        static constexpr std::uint32_t kMaxJointMatrices = 32768;

        // A rectangle of the swapchain image in framebuffer pixels, origin at the top-left.
        struct SceneViewport {
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
        };

        // The window is held by reference for the renderer's lifetime (Engine guarantees it
        // outlives the renderer); non-const because resize notifications are consumed from it.
        explicit Renderer(Window& window);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        Renderer(Renderer&&) = delete;
        Renderer& operator=(Renderer&&) = delete;

        // Records and presents one frame containing everything Submit()ted since the previous call.
        // The submissions are consumed even if no frame is produced (e.g. while minimized).
        void Tick();

        // Queues one draw of `mesh` for the next Tick(): `transform` is its model matrix, and a
        // non-null materialOverrides[i] replaces sub-mesh i's material (see Mesh::Draw). Call once
        // per object per frame. The renderer shares ownership of the mesh and materials until the
        // GPU is done with the frame that draws them. A skinned mesh needs SubmitSkinned (it is
        // ignored here, with a one-time warning).
        void Submit(std::shared_ptr<const Mesh> mesh, const glm::mat4& transform,
                    std::vector<std::shared_ptr<Material>> materialOverrides = {});

        // As Submit, for a skinned mesh (Mesh::IsSkinned) deformed on the GPU by `palette`: its
        // joint matrices (skinning matrices in the mesh's object space), which the mesh's skin
        // stream indexes. The palette is copied. Ignored (with a one-time warning) if the mesh is
        // not skinned, the palette is shorter than the mesh's joint indices need
        // (Mesh::GetJointCount), or the frame's joint buffer is full (kMaxJointMatrices).
        void SubmitSkinned(std::shared_ptr<const Mesh> mesh, const glm::mat4& transform,
                           std::vector<std::shared_ptr<Material>> materialOverrides, std::span<const glm::mat4> palette);

        // Adds a light to the next Tick()'s frame. Call once per light per frame.
        void SubmitLight(const Light& light);

        // The flat ambient light (linear RGB) added to every surface from now on, standing in for
        // indirect/environment lighting. Black until set.
        void SetAmbientLight(const glm::vec3& color) noexcept;

        // Uploads imported geometry as a Mesh, creating a Texture/Material for each of its
        // MeshData::textures/materials and attaching the materials to the sub-meshes that
        // reference them. Blocks until the upload has completed. Throws std::invalid_argument for
        // malformed data (see Mesh).
        [[nodiscard]]
        std::shared_ptr<Mesh> CreateMesh(const MeshData& data) const;

        // As above, for a skinned mesh: `skin` (one SkinnedVertex per vertex) is uploaded as its
        // skin stream (see Mesh). Draw it with SubmitSkinned.
        [[nodiscard]]
        std::shared_ptr<Mesh> CreateMesh(const MeshData& data, std::span<const SkinnedVertex> skin) const;

        // Creates textures and materials (e.g. material overrides built in the editor).
        [[nodiscard]]
        const MaterialSystem& GetMaterialSystem() const noexcept;

        // Restricts scene rendering to a sub-rectangle of the window (clamped to the swapchain
        // extent), or resets it to the whole window (std::nullopt). The camera's aspect ratio then
        // follows the rectangle. The overlay still covers the whole window.
        void SetSceneViewport(const std::optional<SceneViewport>& viewport) noexcept;

        // The camera subsequent frames are rendered from. Copied (a Camera is a small value), so
        // the caller updates it by calling this again, typically once per frame. The projection's
        // aspect ratio comes from the swapchain extent at the time the frame is recorded.
        void SetCamera(const Camera& camera);

        // Registers the overlay recorded on top of the scene each frame, or clears it (nullptr).
        // Non-owning: the overlay must stay alive until it is cleared or the renderer is
        // destroyed, and must itself wait for the GPU before destroying resources that frames in
        // flight may still use.
        void SetOverlay(RenderOverlay* overlay) noexcept;

        // Read-only access for systems that create GPU resources against this renderer's device
        // (e.g. ImGuiLayer). Valid for the renderer's lifetime.
        [[nodiscard]]
        const VulkanContext& GetContext() const noexcept;

        [[nodiscard]]
        const VulkanPhysicalDevice& GetPhysicalDevice() const noexcept;

        [[nodiscard]]
        const VulkanDevice& GetDevice() const noexcept;

        // Color format of the swapchain images (what an overlay renders into). Stable in practice,
        // but may change when the swapchain is rebuilt; overlays receive the current one per frame.
        [[nodiscard]]
        VkFormat GetSwapchainImageFormat() const noexcept;

        // Format of the depth attachment bound alongside the swapchain image. Fixed at startup.
        [[nodiscard]]
        VkFormat GetDepthFormat() const noexcept;

    private:
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

        // Per frame-in-flight uniform buffer holding FrameUniforms, the storage buffer holding the
        // frame's GpuLights (room for kMaxLights), and the set 0 descriptor set pointing at both.
        // Host-visible and rewritten by UpdateFrameUniforms once the slot's fence proves the
        // previous frame that read them has finished.
        struct FrameUniformBuffer {
            std::unique_ptr<VulkanBuffer> buffer;
            std::unique_ptr<VulkanBuffer> lightBuffer;
            std::unique_ptr<VulkanBuffer> jointBuffer;  // Room for kMaxJointMatrices.
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        };

        // One Submit(): what to draw and with what. Holds shared ownership so the mesh/materials
        // outlive every command buffer that references them.
        struct DrawCommand {
            std::shared_ptr<const Mesh> mesh;
            glm::mat4 transform{1.0f};
            std::vector<std::shared_ptr<Material>> materialOverrides;
            std::uint32_t firstJoint = 0;  // Skinned draws: where the palette starts in the joint buffer.
        };

        // Everything a swapchain recreation replaces, kept alive until it is provably unused.
        struct RetiredResources {
            VulkanSwapchain::Retired swapchain;
            std::unique_ptr<VulkanImage> depthImage;   // Sized to that swapchain's extent.
            std::vector<VkSemaphore> renderFinished;   // That swapchain's per-image semaphores.
            std::unique_ptr<VulkanPipeline> pipeline;  // Only if the pipelines had to be rebuilt.
            std::unique_ptr<VulkanPipeline> skinnedPipeline;
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

        // The first depth format (best precision first) the device supports as a depth attachment.
        // D16_UNORM is guaranteed by the spec, so this always succeeds.
        [[nodiscard]]
        VkFormat ChooseDepthFormat() const;

        // (Re)creates m_DepthImage at the swapchain's current extent.
        void CreateDepthImage();

        // The part of the swapchain image the scene is drawn into this frame: m_SceneViewport
        // clamped to the extent, or the whole extent if unset. May be empty (zero area).
        [[nodiscard]]
        VkRect2D GetSceneRect() const noexcept;

        void CreateSyncObjects();
        void DestroySyncObjects() noexcept;

        void CreateFrameUniformBuffers();
        void DestroyFrameUniformBuffers() noexcept;

        // Writes this frame's FrameUniforms (view and projection from m_Camera, or the placeholder
        // if none has been set, the ambient light and the light count), its lights and its joint
        // matrices into the given slot's buffers. The projection is built for the aspect ratio of
        // `sceneRect`.
        void UpdateFrameUniforms(std::uint32_t frameIndex, const VkRect2D& sceneRect, const std::vector<Light>& lights,
                                 const std::vector<glm::mat4>& joints) const;

        // The graphics pipelines (static and skinned) for the current swapchain format.
        void CreatePipelines();

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

        void RecordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                                 const VkRect2D& sceneRect) const;

        // Records a single image layout transition. The stage/access masks must describe the work
        // that produces the image in oldLayout (src) and the work that consumes it in newLayout
        // (dst) so the transition is correctly ordered against both; see RecordCommandBuffer.
        static void TransitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspectMask,
                                    VkImageLayout oldLayout, VkImageLayout newLayout,
                                    VkPipelineStageFlags2 srcStageMask, VkAccessFlags2 srcAccessMask,
                                    VkPipelineStageFlags2 dstStageMask, VkAccessFlags2 dstAccessMask);

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
        // One depth image, shared by all frames in flight: frames execute in submission order on
        // the one graphics queue, and RecordCommandBuffer's barrier orders each frame's depth
        // clear after the previous frame's depth writes. Rebuilt (and the old one retired) with
        // the swapchain, whose extent it must match.
        VkFormat m_DepthFormat = VK_FORMAT_UNDEFINED;
        std::unique_ptr<VulkanImage> m_DepthImage;
        std::unique_ptr<VulkanCommandPool> m_CommandPool;
        // Outlives every Texture (they return their descriptor sets to it) and the pipeline (whose
        // layout is built from its set layout).
        std::unique_ptr<VulkanDescriptorAllocator> m_DescriptorAllocator;
        std::unique_ptr<VulkanPipeline> m_Pipeline;
        std::unique_ptr<VulkanPipeline> m_SkinnedPipeline;  // For skinned meshes; layout-compatible with m_Pipeline.
        // Creates textures/materials and owns the defaults (bound for sub-meshes without a
        // material); references the allocator, device, command pool and descriptor allocator above.
        std::unique_ptr<MaterialSystem> m_MaterialSystem;

        std::optional<Camera> m_Camera;                // Unset until SetCamera(); see UpdateFrameUniforms.
        std::optional<SceneViewport> m_SceneViewport;  // Unset = whole window; see SetSceneViewport.
        RenderOverlay* m_Overlay = nullptr;           // Non-owning; see SetOverlay.

        // Draws submitted since the last Tick(), and per frame-in-flight slot the draws that slot's
        // command buffer was recorded with. A slot's list is only replaced after its fence wait,
        // which is what makes dropping those references safe.
        std::vector<DrawCommand> m_PendingDraws;
        std::vector<std::vector<DrawCommand>> m_FrameDraws;

        // Lights submitted since the last Tick() (copied into the frame's light buffer, so they need
        // no lifetime tracking), and the ambient term.
        std::vector<Light> m_PendingLights;
        glm::vec3 m_AmbientLight{0.0f};
        bool m_LightLimitWarned = false;

        // The palettes of the skinned draws submitted since the last Tick(), back to back (copied
        // into the frame's joint buffer), and one-time warnings about skinned submissions.
        std::vector<glm::mat4> m_PendingJoints;
        bool m_JointLimitWarned = false;
        bool m_SkinnedMisuseWarned = false;

        std::vector<VkCommandBuffer> m_CommandBuffers;
        std::vector<FrameUniformBuffer> m_FrameUniforms;  // One per frame in flight.

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
