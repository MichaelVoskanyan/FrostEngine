#include <Frost/ImGui/ImGuiLayer.hpp>

#include <Frost/Platform/Window.hpp>
#include <Frost/Renderer/Renderer.hpp>
#include <Frost/Renderer/Vulkan/VulkanContext.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>

// The Vulkan backend's init structure has changed shape across releases. 1.91.9 is the oldest
// release with everything used below (backend-owned descriptor pool from 1.91.6, ApiVersion from
// 1.91.9); the one later field move (1.92.4) is handled where the field is set.
#if IMGUI_VERSION_NUM < 19190
#error "Frost requires Dear ImGui 1.91.9 or newer (vcpkg: the 'imgui' port with glfw-binding and vulkan-binding)."
#endif

namespace {

    // Descriptor sets the ImGui Vulkan backend may allocate from its own pool: one per texture
    // (the font atlas, plus any registered with ImGui_ImplVulkan_AddTexture, e.g. a future scene
    // viewport). Generous; descriptors are cheap.
    constexpr std::uint32_t kDescriptorPoolSize = 64;

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << '.';

        return std::runtime_error(message.str());
    }

    // Called by the backend after each of its Vulkan calls. It cannot usefully propagate an error
    // (the call sits inside ImGui's own code), so report it; a fatal condition such as device loss
    // resurfaces through the renderer's own checked calls on the same frame.
    void CheckVkResult(const VkResult result) {
        if (result == VK_SUCCESS) { return; }

        std::cerr << "[Frost::ImGuiLayer] Vulkan call in the ImGui backend returned VkResult "
                  << static_cast<int>(result) << ".\n";
    }

    [[nodiscard]]
    bool IsSrgbFormat(const VkFormat format) noexcept {
        switch (format) {
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return true;
            default: return false;
        }
    }

    [[nodiscard]]
    float SrgbToLinear(const float value) noexcept {
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    }

}  // namespace

namespace Frost {

    ImGuiLayer::ImGuiLayer(Window& window, Renderer& renderer) : m_Renderer(renderer) {
        IMGUI_CHECKVERSION();
        m_Context = ImGui::CreateContext();

        try {
            ImGuiIO& io = ImGui::GetIO();

#ifdef IMGUI_HAS_DOCK
            // Docking is what an editor's panel layout is built on. Multi-viewport (dragging panels
            // out into their own OS windows) is left off: it needs the renderer to drive extra
            // swapchains.
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif

            ApplyStyle(renderer.GetSwapchainImageFormat());

            // install_callbacks = true: the backend installs its GLFW callbacks and forwards each
            // event to the one previously installed (Window's), so both keep working.
            if (!ImGui_ImplGlfw_InitForVulkan(window.GetNativeHandle(), true)) {
                throw std::runtime_error("ImGui_ImplGlfw_InitForVulkan failed.");
            }
            m_PlatformBackendInitialized = true;

            InitRendererBackend(renderer.GetSwapchainImageFormat(), renderer.GetDepthFormat());
        }
        catch (...) {
            Shutdown();
            throw;
        }

        m_Renderer.SetOverlay(this);
    }

    ImGuiLayer::~ImGuiLayer() {
        m_Renderer.SetOverlay(nullptr);

        // Frames still in flight may reference the backend's pipeline, vertex/index buffers and
        // font texture. Shutdown is the one place a full idle is appropriate.
        const VkResult result = vkDeviceWaitIdle(m_Renderer.GetDevice().GetHandle());
        if (result != VK_SUCCESS) {
            std::cerr << "[Frost::ImGuiLayer] vkDeviceWaitIdle failed with VkResult " << static_cast<int>(result)
                      << " during shutdown; tearing down anyway.\n";
        }

        Shutdown();
    }

    void ImGuiLayer::BeginFrame() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiLayer::EndFrame() { ImGui::Render(); }

    void ImGuiLayer::RecordOverlay(const VkCommandBuffer commandBuffer, const RenderOverlayTarget& target) {
        if (target.colorAttachmentFormat != m_ColorAttachmentFormat ||
            target.depthAttachmentFormat != m_DepthAttachmentFormat) {
            // The swapchain was rebuilt with a different format (the renderer rebuilds its own
            // pipeline for this too; in practice it does not happen for a given surface). The
            // backend's pipeline is baked against the formats, so rebuild the backend. Its old
            // objects may still be used by frames in flight, hence the idle; rare enough not to
            // matter. Nothing is drawn this frame: this frame's draw data may reference textures
            // the old backend owned.
            const VkResult result = vkDeviceWaitIdle(m_Renderer.GetDevice().GetHandle());
            if (result != VK_SUCCESS) { throw VulkanError("vkDeviceWaitIdle", result); }

            ImGui_ImplVulkan_Shutdown();
            m_RendererBackendInitialized = false;

            InitRendererBackend(target.colorAttachmentFormat, target.depthAttachmentFormat);
            ApplyStyle(target.colorAttachmentFormat);  // Takes effect from the next frame.
            return;
        }

        ImDrawData* const drawData = ImGui::GetDrawData();
        if (drawData == nullptr) { return; }  // No frame has been finalized yet.

        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
    }

    void ImGuiLayer::InitRendererBackend(const VkFormat colorAttachmentFormat, const VkFormat depthAttachmentFormat) {
        m_ColorAttachmentFormat = colorAttachmentFormat;
        m_DepthAttachmentFormat = depthAttachmentFormat;

        // ImGui draws with depth testing off, but its pipeline still has to declare the depth
        // attachment the renderer's rendering scope has (see RenderOverlayTarget).
        const VkPipelineRenderingCreateInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .pNext = nullptr,
                .viewMask = 0,
                .colorAttachmentCount = 1,
                .pColorAttachmentFormats = &m_ColorAttachmentFormat,
                .depthAttachmentFormat = depthAttachmentFormat,
                .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };

        // Zero-initialized as the backend requires; only what differs from zero is set.
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VulkanContext::kApiVersion;
        info.Instance = m_Renderer.GetContext().GetInstance();
        info.PhysicalDevice = m_Renderer.GetPhysicalDevice().GetHandle();
        info.Device = m_Renderer.GetDevice().GetHandle();
        info.QueueFamily = m_Renderer.GetPhysicalDevice().GetQueueFamilyIndices().graphicsFamily.value();
        info.Queue = m_Renderer.GetDevice().GetGraphicsQueue();
        info.DescriptorPoolSize = kDescriptorPoolSize;  // Let the backend own its descriptor pool.

        // ImageCount is how many copies of its per-frame vertex/index buffers the backend rotates
        // through, and how many frames a texture must go unused before it is freed: it has to
        // cover the renderer's frames in flight. It is deliberately not the swapchain image count,
        // which can change on a rebuild while this value is fixed at init. (MinImageCount only
        // matters for multi-viewport swapchains, which are not enabled.)
        info.MinImageCount = 2;
        info.ImageCount = std::max<std::uint32_t>(info.MinImageCount, Renderer::kFramesInFlight);

        // The renderer uses dynamic rendering, and ImGui is drawn inside its rendering scope.
        info.UseDynamicRendering = true;
#if IMGUI_VERSION_NUM >= 19240
        info.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
#else
        info.PipelineRenderingCreateInfo = renderingInfo;
#endif

        info.CheckVkResultFn = CheckVkResult;

        if (!ImGui_ImplVulkan_Init(&info)) { throw std::runtime_error("ImGui_ImplVulkan_Init failed."); }
        m_RendererBackendInitialized = true;
    }

    void ImGuiLayer::ApplyStyle(const VkFormat colorAttachmentFormat) {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::StyleColorsDark(&style);

        // ImGui's colors are sRGB-encoded values, and its shader writes them out unchanged. Into an
        // sRGB attachment (the renderer prefers one) the hardware would then encode them a second
        // time on write, washing the whole UI out. Converting the palette to linear once makes it
        // come out as designed. (Blending then happens in linear space, which slightly changes how
        // translucent panels look; that is the accepted trade-off of this approach.)
        if (!IsSrgbFormat(colorAttachmentFormat)) { return; }

        for (ImVec4& color : style.Colors) {
            color.x = SrgbToLinear(color.x);
            color.y = SrgbToLinear(color.y);
            color.z = SrgbToLinear(color.z);
        }
    }

    void ImGuiLayer::Shutdown() noexcept {
        if (m_RendererBackendInitialized) {
            ImGui_ImplVulkan_Shutdown();
            m_RendererBackendInitialized = false;
        }

        if (m_PlatformBackendInitialized) {
            ImGui_ImplGlfw_Shutdown();
            m_PlatformBackendInitialized = false;
        }

        if (m_Context != nullptr) {
            ImGui::DestroyContext(m_Context);
            m_Context = nullptr;
        }
    }

}  // namespace Frost
