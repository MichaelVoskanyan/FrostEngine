#pragma once

#include <Frost/Project/GameLaunchOptions.hpp>
#include <Frost/Renderer/Camera.hpp>
#include <Frost/Scene/Bounds.hpp>

#include <glm/vec3.hpp>

namespace Frost {
    class Input;
}

// The game's free-flying camera, for levels that have no camera of their own (like the default
// pawn Unreal spawns when playing a level without a player start):
//   - click in the window to take the mouse (captured and hidden); then the mouse looks around;
//   - Escape gives the mouse back;
//   - W/A/S/D move along the view and sideways, Space / Left Ctrl straight up and down, Left Shift
//     moves faster, and the mouse wheel (while the mouse is taken) changes the speed.
// Orientation is yaw and pitch, as in Frost::ViewPoint (no roll; pitch stops short of straight up
// or down).
class SpectatorCamera final {
public:
    SpectatorCamera();

    // Looks from `view`.
    void SetView(const Frost::ViewPoint& view);

    // Keeps the direction, and moves back until all of `bounds` is in view.
    void Frame(const Frost::Bounds& bounds);

    void OnUpdate(float deltaTime, Frost::Input& input);

    // Whether the mouse is taken (captured for looking around).
    [[nodiscard]]
    bool HasMouse() const noexcept;

    // Gives the mouse back, if it was taken.
    void ReleaseMouse(Frost::Input& input);

    [[nodiscard]]
    const Frost::Camera& GetCamera() const noexcept;

private:
    void SyncCamera();

    Frost::Camera m_Camera;
    glm::vec3 m_Position;
    float m_Yaw;    // Radians.
    float m_Pitch;  // Radians.
    float m_MoveSpeed;
    bool m_HasMouse = false;
};
