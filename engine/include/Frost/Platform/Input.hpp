#pragma once

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>

namespace Frost {

    class Window;

    // Keyboard keys. The values are GLFW's key codes (static_asserted in Input.cpp), which keeps
    // GLFW itself out of this header while making the mapping a no-op. Extend as needed.
    enum class Key : std::int16_t {
        Space = 32,

        D0 = 48, D1, D2, D3, D4, D5, D6, D7, D8, D9,

        A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

        Escape = 256,
        Enter = 257,
        Tab = 258,
        Backspace = 259,
        Insert = 260,
        Delete = 261,
        Right = 262,
        Left = 263,
        Down = 264,
        Up = 265,
        PageUp = 266,
        PageDown = 267,
        Home = 268,
        End = 269,

        F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

        LeftShift = 340,
        LeftControl = 341,
        LeftAlt = 342,
        LeftSuper = 343,
        RightShift = 344,
        RightControl = 345,
        RightAlt = 346,
        RightSuper = 347,
    };

    // Mouse buttons; values are GLFW's button indices.
    enum class MouseButton : std::uint8_t {
        Left = 0,
        Right = 1,
        Middle = 2,
    };

    enum class CursorMode : std::uint8_t {
        Normal,    // Visible and free to leave the window.
        Hidden,    // Invisible while over the window, but otherwise free.
        Captured,  // Invisible and locked to the window; the mouse delta is unbounded (mouse-look).
    };

    // Per-frame snapshot of keyboard and mouse state for one window. Engine calls Update() once per
    // frame, right after the window's events are polled, so everything queried during that frame
    // sees one consistent state: "down" is the state at the snapshot, "pressed"/"released" mean the
    // state changed since the previous snapshot, and the mouse/scroll deltas cover exactly the time
    // between the two. A key or button pressed and released again between two snapshots still
    // reads as down (and pressed) in the next one, so short taps during long frames are not lost.
    //
    // Input only reads state; it does not consume it. Whether a UI layer (ImGui) should get a given
    // click or key instead of the game/editor is a policy decision left to the caller.
    class Input final {
    public:
        // The window must outlive this object (Engine guarantees it).
        explicit Input(Window& window);

        Input(const Input&) = delete;
        Input& operator=(const Input&) = delete;

        Input(Input&&) = delete;
        Input& operator=(Input&&) = delete;

        // Takes this frame's snapshot. Called by Engine; not meant for application code.
        void Update();

        [[nodiscard]]
        bool IsKeyDown(Key key) const noexcept;

        // True only on the frame the key went down / came back up.
        [[nodiscard]]
        bool WasKeyPressed(Key key) const noexcept;

        [[nodiscard]]
        bool WasKeyReleased(Key key) const noexcept;

        [[nodiscard]]
        bool IsMouseButtonDown(MouseButton button) const noexcept;

        [[nodiscard]]
        bool WasMouseButtonPressed(MouseButton button) const noexcept;

        [[nodiscard]]
        bool WasMouseButtonReleased(MouseButton button) const noexcept;

        // Cursor position in screen coordinates relative to the window's top-left corner (+Y down).
        // While captured this is a virtual, unbounded position; only its delta is meaningful.
        [[nodiscard]]
        glm::vec2 GetMousePosition() const noexcept;

        // Cursor movement since the previous frame, in the same units (+X right, +Y down). Zero on
        // the frame the cursor mode changes, so mode switches never read as a sudden jump.
        [[nodiscard]]
        glm::vec2 GetMouseDelta() const noexcept;

        // Scroll accumulated since the previous frame, in wheel notches (fractional on touchpads).
        // y > 0 is the wheel pushed away from the user ("up").
        [[nodiscard]]
        glm::vec2 GetScrollDelta() const noexcept;

        // Captured hides the cursor and locks it to the window (GLFW_CURSOR_DISABLED), using raw
        // (unaccelerated) mouse motion where the platform supports it. Takes effect immediately.
        void SetCursorMode(CursorMode mode);

        [[nodiscard]]
        CursorMode GetCursorMode() const noexcept;

    private:
        // Covers every GLFW key code (GLFW_KEY_LAST == 348) and mouse button (GLFW_MOUSE_BUTTON_LAST == 7);
        // static_asserted in Input.cpp.
        static constexpr std::size_t kKeyCount = 349;
        static constexpr std::size_t kMouseButtonCount = 8;

        // Reads the cursor position from the window into m_MousePosition (no delta).
        void SampleCursorPosition();

        Window& m_Window;

        std::array<bool, kKeyCount> m_Keys{};
        std::array<bool, kKeyCount> m_PreviousKeys{};
        std::array<bool, kMouseButtonCount> m_MouseButtons{};
        std::array<bool, kMouseButtonCount> m_PreviousMouseButtons{};

        glm::vec2 m_MousePosition{0.0f};
        glm::vec2 m_MouseDelta{0.0f};
        glm::vec2 m_ScrollDelta{0.0f};

        CursorMode m_CursorMode = CursorMode::Normal;

        // Set by SetCursorMode: the next Update() re-bases the cursor position instead of reporting
        // a delta. Entering/leaving GLFW_CURSOR_DISABLED warps or re-bases the (virtual) cursor,
        // which would otherwise show up as one large bogus movement.
        bool m_DiscardNextMouseDelta = true;
    };

}  // namespace Frost
