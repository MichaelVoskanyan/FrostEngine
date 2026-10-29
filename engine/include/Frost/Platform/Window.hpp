#pragma once

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

        void PollEvents() const noexcept;

        // Size of the window's drawable surface in pixels, as reported by GLFW. This can differ
        // from the WindowSpecification width/height (DPI scaling, OS-imposed constraints), so it
        // is what the swapchain extent should be derived from rather than m_Spec.
        [[nodiscard]]
        std::pair<std::uint32_t, std::uint32_t> GetFramebufferSize() const noexcept;

        [[nodiscard]]
        GLFWwindow* GetNativeHandle() const noexcept;

    private:
        static void ErrorCallback(int errorCode, const char* description);

        GLFWwindow* m_Window = nullptr;
        WindowSpecification m_Spec;
    };

}  // namespace Frost
