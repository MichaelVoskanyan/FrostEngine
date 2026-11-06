#include <Frost/Platform/Input.hpp>

#include <Frost/Platform/Window.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstddef>

namespace {

    // Key and MouseButton use GLFW's codes directly. Ranges that are contiguous in both are checked
    // at their ends; everything else individually.
    static_assert(static_cast<int>(Frost::Key::Space) == GLFW_KEY_SPACE);
    static_assert(static_cast<int>(Frost::Key::D0) == GLFW_KEY_0);
    static_assert(static_cast<int>(Frost::Key::D9) == GLFW_KEY_9);
    static_assert(static_cast<int>(Frost::Key::A) == GLFW_KEY_A);
    static_assert(static_cast<int>(Frost::Key::Z) == GLFW_KEY_Z);
    static_assert(static_cast<int>(Frost::Key::Escape) == GLFW_KEY_ESCAPE);
    static_assert(static_cast<int>(Frost::Key::Enter) == GLFW_KEY_ENTER);
    static_assert(static_cast<int>(Frost::Key::Tab) == GLFW_KEY_TAB);
    static_assert(static_cast<int>(Frost::Key::Backspace) == GLFW_KEY_BACKSPACE);
    static_assert(static_cast<int>(Frost::Key::Insert) == GLFW_KEY_INSERT);
    static_assert(static_cast<int>(Frost::Key::Delete) == GLFW_KEY_DELETE);
    static_assert(static_cast<int>(Frost::Key::Right) == GLFW_KEY_RIGHT);
    static_assert(static_cast<int>(Frost::Key::Left) == GLFW_KEY_LEFT);
    static_assert(static_cast<int>(Frost::Key::Down) == GLFW_KEY_DOWN);
    static_assert(static_cast<int>(Frost::Key::Up) == GLFW_KEY_UP);
    static_assert(static_cast<int>(Frost::Key::PageUp) == GLFW_KEY_PAGE_UP);
    static_assert(static_cast<int>(Frost::Key::PageDown) == GLFW_KEY_PAGE_DOWN);
    static_assert(static_cast<int>(Frost::Key::Home) == GLFW_KEY_HOME);
    static_assert(static_cast<int>(Frost::Key::End) == GLFW_KEY_END);
    static_assert(static_cast<int>(Frost::Key::F1) == GLFW_KEY_F1);
    static_assert(static_cast<int>(Frost::Key::F12) == GLFW_KEY_F12);
    static_assert(static_cast<int>(Frost::Key::LeftShift) == GLFW_KEY_LEFT_SHIFT);
    static_assert(static_cast<int>(Frost::Key::LeftControl) == GLFW_KEY_LEFT_CONTROL);
    static_assert(static_cast<int>(Frost::Key::LeftAlt) == GLFW_KEY_LEFT_ALT);
    static_assert(static_cast<int>(Frost::Key::LeftSuper) == GLFW_KEY_LEFT_SUPER);
    static_assert(static_cast<int>(Frost::Key::RightShift) == GLFW_KEY_RIGHT_SHIFT);
    static_assert(static_cast<int>(Frost::Key::RightControl) == GLFW_KEY_RIGHT_CONTROL);
    static_assert(static_cast<int>(Frost::Key::RightAlt) == GLFW_KEY_RIGHT_ALT);
    static_assert(static_cast<int>(Frost::Key::RightSuper) == GLFW_KEY_RIGHT_SUPER);

    static_assert(static_cast<int>(Frost::MouseButton::Left) == GLFW_MOUSE_BUTTON_LEFT);
    static_assert(static_cast<int>(Frost::MouseButton::Right) == GLFW_MOUSE_BUTTON_RIGHT);
    static_assert(static_cast<int>(Frost::MouseButton::Middle) == GLFW_MOUSE_BUTTON_MIDDLE);

    // glfwGetKey accepts any code in [GLFW_KEY_SPACE, GLFW_KEY_LAST] (codes in the gaps simply
    // read as released); anything below is an error.
    constexpr int kFirstPolledKey = GLFW_KEY_SPACE;

}  // namespace

namespace Frost {

    Input::Input(Window& window) : m_Window(window) {
        // The state arrays are indexed directly by GLFW code (declared in the header without GLFW).
        static_assert(kKeyCount == static_cast<std::size_t>(GLFW_KEY_LAST) + 1);
        static_assert(kMouseButtonCount == static_cast<std::size_t>(GLFW_MOUSE_BUTTON_LAST) + 1);

        // Keys and buttons are sampled once per frame, so a tap that starts and ends between two
        // samples (a quick press during a long frame, e.g. while a level loads) would be missed
        // entirely. Sticky mode makes GLFW report such a press as down at the next sample, so it
        // still shows up for one frame; held keys are unaffected.
        GLFWwindow* const handle = m_Window.GetNativeHandle();
        glfwSetInputMode(handle, GLFW_STICKY_KEYS, GLFW_TRUE);
        glfwSetInputMode(handle, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);

        SampleCursorPosition();
    }

    void Input::Update() {
        GLFWwindow* const window = m_Window.GetNativeHandle();

        m_PreviousKeys = m_Keys;
        for (int key = kFirstPolledKey; key < static_cast<int>(kKeyCount); ++key) {
            m_Keys[static_cast<std::size_t>(key)] = glfwGetKey(window, key) == GLFW_PRESS;
        }

        m_PreviousMouseButtons = m_MouseButtons;
        for (int button = 0; button < static_cast<int>(kMouseButtonCount); ++button) {
            m_MouseButtons[static_cast<std::size_t>(button)] = glfwGetMouseButton(window, button) == GLFW_PRESS;
        }

        // Position is sampled rather than accumulated from events: the difference of two samples is
        // exactly the movement between frames however many motion events arrived in between.
        const glm::vec2 previousPosition = m_MousePosition;
        SampleCursorPosition();

        if (m_DiscardNextMouseDelta) {
            m_MouseDelta = glm::vec2(0.0f);
            m_DiscardNextMouseDelta = false;
        }
        else {
            m_MouseDelta = m_MousePosition - previousPosition;
        }

        m_ScrollDelta = m_Window.ConsumeScrollDelta();
    }

    bool Input::IsKeyDown(const Key key) const noexcept { return m_Keys[static_cast<std::size_t>(key)]; }

    bool Input::WasKeyPressed(const Key key) const noexcept {
        const auto index = static_cast<std::size_t>(key);
        return m_Keys[index] && !m_PreviousKeys[index];
    }

    bool Input::WasKeyReleased(const Key key) const noexcept {
        const auto index = static_cast<std::size_t>(key);
        return !m_Keys[index] && m_PreviousKeys[index];
    }

    bool Input::IsMouseButtonDown(const MouseButton button) const noexcept {
        return m_MouseButtons[static_cast<std::size_t>(button)];
    }

    bool Input::WasMouseButtonPressed(const MouseButton button) const noexcept {
        const auto index = static_cast<std::size_t>(button);
        return m_MouseButtons[index] && !m_PreviousMouseButtons[index];
    }

    bool Input::WasMouseButtonReleased(const MouseButton button) const noexcept {
        const auto index = static_cast<std::size_t>(button);
        return !m_MouseButtons[index] && m_PreviousMouseButtons[index];
    }

    glm::vec2 Input::GetMousePosition() const noexcept { return m_MousePosition; }

    glm::vec2 Input::GetMouseDelta() const noexcept { return m_MouseDelta; }

    glm::vec2 Input::GetScrollDelta() const noexcept { return m_ScrollDelta; }

    void Input::SetCursorMode(const CursorMode mode) {
        if (mode == m_CursorMode) { return; }

        GLFWwindow* const window = m_Window.GetNativeHandle();

        int glfwMode = GLFW_CURSOR_NORMAL;
        switch (mode) {
            case CursorMode::Normal: glfwMode = GLFW_CURSOR_NORMAL; break;
            case CursorMode::Hidden: glfwMode = GLFW_CURSOR_HIDDEN; break;
            case CursorMode::Captured: glfwMode = GLFW_CURSOR_DISABLED; break;
        }

        // Raw motion bypasses OS pointer acceleration, which is what mouse-look wants. GLFW only
        // delivers it while the cursor is disabled, and not every platform/session supports it.
        if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, mode == CursorMode::Captured ? GLFW_TRUE : GLFW_FALSE);
        }

        glfwSetInputMode(window, GLFW_CURSOR, glfwMode);

        m_CursorMode = mode;
        m_DiscardNextMouseDelta = true;
    }

    CursorMode Input::GetCursorMode() const noexcept { return m_CursorMode; }

    void Input::SampleCursorPosition() {
        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(m_Window.GetNativeHandle(), &x, &y);
        m_MousePosition = glm::vec2(static_cast<float>(x), static_cast<float>(y));
    }

}  // namespace Frost
