#pragma once

#include <cstdint>
#include <string>

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

        [[nodiscard]]
        GLFWwindow* GetNativeHandle() const noexcept;

    private:
        static void ErrorCallback(int errorCode, const char* description);

        GLFWwindow* m_Window = nullptr;
        WindowSpecification m_Spec;
    };

}  // namespace Frost
