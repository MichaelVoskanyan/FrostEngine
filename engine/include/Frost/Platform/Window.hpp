#pragma once

#include <glm/vec2.hpp>

#include <cstdint>
#include <string>
#include <utility>

struct GLFWwindow;

namespace Frost {

    struct WindowSpecification {
        std::string title = "Frost Engine";
        std::uint32_t width = 1280;
        std::uint32_t height = 720;
        bool resizable = true;
    };

    class Window final {
    public:
        explicit Window(const WindowSpecification& spec);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        Window(Window&&) = delete;
        Window& operator=(Window&&) = delete;

        [[nodiscard]]
        bool ShouldClose() const noexcept;

        // Sets or cancels a pending close (the user clicking the window's close button sets it).
        // Cancelling lets an application ask first, e.g. about unsaved changes.
        void SetShouldClose(bool shouldClose) noexcept;

        void SetTitle(const std::string& title);

        void PollEvents() const noexcept;

        // Size of the window's drawable surface in pixels, as reported by GLFW. This can differ
        // from the WindowSpecification width/height (DPI scaling, OS-imposed constraints), so it
        // is what the swapchain extent should be derived from rather than m_Spec.
        [[nodiscard]]
        std::pair<std::uint32_t, std::uint32_t> GetFramebufferSize() const noexcept;

        // Returns true if the framebuffer has been resized since the last call, and clears the
        // flag. The swapchain owner polls this once per frame: a size change is the one window
        // event that invalidates GPU resources, and the presentation engine alone is not a reliable
        // source for it (some drivers never report VK_ERROR_OUT_OF_DATE_KHR on resize).
        [[nodiscard]]
        bool ConsumeFramebufferResize() noexcept;

        // Returns the scroll offset (mouse wheel / touchpad) accumulated since the last call, and
        // resets it. x is horizontal, y vertical (positive = wheel pushed away from the user).
        // GLFW only reports scrolling as events, so it is accumulated here between polls; Input
        // consumes it once per frame.
        [[nodiscard]]
        glm::vec2 ConsumeScrollDelta() noexcept;

        [[nodiscard]]
        GLFWwindow* GetNativeHandle() const noexcept;

    private:
        static void ErrorCallback(int errorCode, const char* description);
        static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
        static void ScrollCallback(GLFWwindow* window, double xOffset, double yOffset);

        GLFWwindow* m_Window = nullptr;
        WindowSpecification m_Spec;
        bool m_FramebufferResized = false;
        glm::vec2 m_ScrollDelta{0.0f};
    };

}  // namespace Frost
