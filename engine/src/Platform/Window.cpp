#include <Frost/Platform/Window.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>
#include <stdexcept>
#include <utility>

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

        // GLFW callbacks are plain function pointers; the user pointer is how the static callback
        // finds its way back to this instance. These must be installed before anything that chains
        // onto them (ImGuiLayer's GLFW backend saves and forwards to whatever is installed when it
        // initializes), which Engine's construction order guarantees.
        glfwSetWindowUserPointer(m_Window, this);
        glfwSetFramebufferSizeCallback(m_Window, FramebufferSizeCallback);
        glfwSetScrollCallback(m_Window, ScrollCallback);
    }

    Window::~Window() {
        if (m_Window != nullptr) {
            glfwDestroyWindow(m_Window);
            m_Window = nullptr;
        }

        glfwTerminate();
    }

    bool Window::ShouldClose() const noexcept { return glfwWindowShouldClose(m_Window) == GLFW_TRUE; }

    void Window::SetShouldClose(const bool shouldClose) noexcept {
        glfwSetWindowShouldClose(m_Window, shouldClose ? GLFW_TRUE : GLFW_FALSE);
    }

    void Window::SetTitle(const std::string& title) {
        m_Spec.title = title;
        glfwSetWindowTitle(m_Window, title.c_str());
    }

    void Window::PollEvents() const noexcept { glfwPollEvents(); }

    std::pair<std::uint32_t, std::uint32_t> Window::GetFramebufferSize() const noexcept {
        int width = 0;
        int height = 0;

        glfwGetFramebufferSize(m_Window, &width, &height);

        return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    }

    bool Window::ConsumeFramebufferResize() noexcept {
        const bool resized = m_FramebufferResized;
        m_FramebufferResized = false;
        return resized;
    }

    glm::vec2 Window::ConsumeScrollDelta() noexcept {
        const glm::vec2 delta = m_ScrollDelta;
        m_ScrollDelta = glm::vec2(0.0f);
        return delta;
    }

    GLFWwindow* Window::GetNativeHandle() const noexcept { return m_Window; }

    void Window::ErrorCallback(const int errorCode, const char* const description) {
        std::cerr << "[GLFW] Error " << errorCode << ": " << (description != nullptr ? description : "Unknown error")
                  << '\n';
    }

    void Window::FramebufferSizeCallback(GLFWwindow* const window, int /*width*/, int /*height*/) {
        // Only the fact that a resize happened is recorded; the new size is read back through
        // GetFramebufferSize() when the swapchain is actually rebuilt, so a burst of resize events
        // collapses into a single recreation.
        auto* const self = static_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self != nullptr) { self->m_FramebufferResized = true; }
    }

    void Window::ScrollCallback(GLFWwindow* const window, const double xOffset, const double yOffset) {
        // Several events can arrive within one poll (a fast wheel spin, or a touchpad's stream of
        // fractional offsets); accumulate them so none are lost between frames.
        auto* const self = static_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self != nullptr) { self->m_ScrollDelta += glm::vec2(static_cast<float>(xOffset), static_cast<float>(yOffset)); }
    }

}  // namespace Frost
