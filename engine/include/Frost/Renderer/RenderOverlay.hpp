#pragma once

#include <vulkan/vulkan.h>

namespace Frost {

    // What an overlay is drawn into: the frame's swapchain image, bound as the single color
    // attachment of the renderer's dynamic-rendering scope, together with the scene's depth
    // attachment (no stencil, one sample). Pipelines used by an overlay must be created against
    // exactly these formats, even if they do not test or write depth (Vulkan requires the
    // pipeline's attachment formats to match the rendering scope's).
    struct RenderOverlayTarget {
        VkFormat colorAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
    };

    // Something drawn on top of the rendered scene each frame (the editor's ImGui UI, later a
    // game's HUD/debug UI). Registered with Renderer::SetOverlay. RecordOverlay is called while
    // the frame's command buffer is being recorded, inside the rendering scope and after all
    // scene draws, so whatever it records lands on top. The overlay must set every piece of
    // dynamic state and every binding it relies on itself (the scene's viewport is vertically
    // flipped, for one), and must leave the rendering scope open.
    //
    // The frame is submitted up to Renderer::kFramesInFlight frames before its GPU work is
    // known to be complete, so per-frame GPU resources the overlay writes must be multi-buffered
    // at least that deep.
    class RenderOverlay {
    public:
        RenderOverlay() = default;
        virtual ~RenderOverlay() = default;

        RenderOverlay(const RenderOverlay&) = delete;
        RenderOverlay& operator=(const RenderOverlay&) = delete;

        RenderOverlay(RenderOverlay&&) = delete;
        RenderOverlay& operator=(RenderOverlay&&) = delete;

        virtual void RecordOverlay(VkCommandBuffer commandBuffer, const RenderOverlayTarget& target) = 0;
    };

}  // namespace Frost
