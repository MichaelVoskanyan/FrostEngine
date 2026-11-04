#pragma once

#include <Frost/Renderer/RenderOverlay.hpp>

#include <vulkan/vulkan.h>

struct ImGuiContext;

namespace Frost {

    class Window;
    class Renderer;

    // Owns the Dear ImGui context and its two backends: GLFW for input/platform, Vulkan for
    // rendering. Created by Engine when EngineSpecification::enableImGui is set; applications then
    // just call ImGui:: functions between Engine::BeginFrame() and Engine::EndFrame() (in practice,
    // from Application::OnImGuiRender()).
    //
    // Frame flow: BeginFrame() starts an ImGui frame (after the window's events were polled),
    // EndFrame() finalizes it into draw data, and the renderer then calls RecordOverlay() while
    // recording its frame, which draws that data on top of the scene. The layer registers itself
    // as the renderer's overlay for its whole lifetime.
    //
    // The GLFW backend chains onto the window's own callbacks (it is created after the Window has
    // installed them), so Window/Input keep seeing every event. Deciding whether the UI or the
    // application should act on an input (ImGuiIO::WantCaptureMouse/WantCaptureKeyboard) is up
    // to the application.
    class ImGuiLayer final : public RenderOverlay {
    public:
        // Both must outlive the layer (Engine guarantees it); the window is only needed here, the
        // GLFW backend keeps its own handle.
        ImGuiLayer(Window& window, Renderer& renderer);
        ~ImGuiLayer() override;

        void BeginFrame();
        void EndFrame();

        void RecordOverlay(VkCommandBuffer commandBuffer, const RenderOverlayTarget& target) override;

    private:
        // (Re)initializes the ImGui Vulkan backend to render into targets of the given formats.
        void InitRendererBackend(VkFormat colorAttachmentFormat, VkFormat depthAttachmentFormat);

        // Resets the style to the default dark theme, converted for the target's color encoding.
        static void ApplyStyle(VkFormat colorAttachmentFormat);

        // Shuts down whatever has been initialized so far, in reverse order. Safe on a partially
        // constructed layer.
        void Shutdown() noexcept;

        Renderer& m_Renderer;

        ImGuiContext* m_Context = nullptr;
        bool m_PlatformBackendInitialized = false;
        bool m_RendererBackendInitialized = false;

        // The formats the backend's pipeline was built for. The color format must outlive the
        // backend: older ImGui versions keep a pointer to it (pColorAttachmentFormats).
        VkFormat m_ColorAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkFormat m_DepthAttachmentFormat = VK_FORMAT_UNDEFINED;
    };

}  // namespace Frost
