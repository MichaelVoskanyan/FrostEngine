#include <Frost/Platform/Window.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>
#include <stdexcept>

namespace Frost {

    Window::Window(const WindowSpecification& spec) : m_Spec(spec) {
        glfwSetErrorCallback(ErrorCallback);

        if (glfwInit() != GLFW_TRUE) { throw std::runtime_error("Failed to initialize GLFW"); }

        // Vulkan requires no GLFW API
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        glfwWindowHint(GLFW_RESIZABLE, m_Spec.resizable ? GLFW_TRUE : GLFW_FALSE);

        m_Window = glfwCreateWindow(static_cast<int>(m_Spec.width), static_cast<int>(m_Spec.height),
                                    m_Spec.title.c_str(), nullptr, nullptr);

        if (m_Window == nullptr) {
            glfwTerminate();
            throw std::runtime_error("Failed to create GLFW window");
        }
    }

    Window::~Window() {
        if (m_Window != nullptr) {
            glfwDestroyWindow(m_Window);
            m_Window = nullptr;
        }

        glfwTerminate();
    }

    bool Window::ShouldClose() const noexcept { return glfwWindowShouldClose(m_Window) == GLFW_TRUE; }

    void Window::PollEvents() const noexcept { glfwPollEvents(); }

    GLFWwindow* Window::GetNativeHandle() const noexcept { return m_Window; }

    void Window::ErrorCallback(const int errorCode, const char* const description) {
        std::cerr << "[GLFW] Error " << errorCode << ": " << (description != nullptr ? description : "Unknown error")
                  << '\n';
    }

}  // namespace Frost
