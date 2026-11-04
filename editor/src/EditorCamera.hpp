#pragma once

#include <Frost/Renderer/Camera.hpp>

#include <glm/vec3.hpp>

namespace Frost {
    class Input;
}

// Free-flying editor viewport camera, driven Unreal-editor style: holding the right mouse button
// takes control (the cursor is captured and hidden), and while held the mouse looks around, WASD
// moves along the view direction and its right vector, Space/Left Ctrl move straight up/down in
// world space, and the mouse wheel scales the movement speed. Releasing the button gives the
// cursor back. Nothing moves without the button held.
//
// Orientation is kept as yaw (about world +Y, positive turns left) and pitch (positive looks
// up), clamped short of straight up/down so the view never flips; there is no roll.
class EditorCamera final {
public:
    EditorCamera();

    // Moves the camera back along its current view direction until a sphere of `radius` around
    // `center` fills the view, keeping the orientation. Ignored while the user is flying.
    void Focus(const glm::vec3& center, float radius);

    // Processes one frame of input. allowBeginControl says whether a right-button press this frame
    // may take control; the caller passes false when the press belongs to something else (e.g. it
    // landed on a UI panel). It does not affect control that is already active.
    void OnUpdate(float deltaTime, Frost::Input& input, bool allowBeginControl);

    // True while the right mouse button is held and the camera owns the (captured) cursor.
    [[nodiscard]]
    bool IsControlling() const noexcept;

    [[nodiscard]]
    const Frost::Camera& GetCamera() const noexcept;

    [[nodiscard]]
    const glm::vec3& GetPosition() const noexcept;

    // Degrees, for display.
    [[nodiscard]]
    float GetYawDegrees() const noexcept;

    [[nodiscard]]
    float GetPitchDegrees() const noexcept;

    // World units per second.
    [[nodiscard]]
    float GetMoveSpeed() const noexcept;

private:
    void BeginControl(Frost::Input& input);
    void EndControl(Frost::Input& input);

    // Pushes m_Position/m_Yaw/m_Pitch into m_Camera.
    void SyncCamera();

    Frost::Camera m_Camera;

    glm::vec3 m_Position;
    float m_Yaw = 0.0f;    // Radians.
    float m_Pitch = 0.0f;  // Radians.
    float m_MoveSpeed;

    bool m_Controlling = false;
};
