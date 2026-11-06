#include "SpectatorCamera.hpp"

#include <Frost/Platform/Input.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>

namespace {

    // Without a level to frame: up and back from the origin, looking slightly down at it.
    constexpr glm::vec3 kStartPosition{0.0f, 2.0f, 6.0f};
    constexpr float kStartPitch = glm::radians(-15.0f);

    // Framing leaves this much room around the level.
    constexpr float kFrameMargin = 1.25f;

    // Radians of rotation per screen unit of mouse movement (the editor camera's).
    constexpr float kLookSensitivity = 0.002f;
    constexpr float kMaxPitch = glm::radians(89.0f);

    // World units per second; the wheel scales it by kSpeedStep per notch, Shift by kFastFactor.
    constexpr float kDefaultMoveSpeed = 3.0f;
    constexpr float kMinMoveSpeed = 0.05f;
    constexpr float kMaxMoveSpeed = 500.0f;
    constexpr float kSpeedStep = 1.2f;
    constexpr float kFastFactor = 4.0f;

    // A long frame (a hitch, the window being dragged) does not teleport the camera.
    constexpr float kMaxDeltaTime = 0.1f;

    constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

}  // namespace

SpectatorCamera::SpectatorCamera()
    : m_Position(kStartPosition), m_Yaw(0.0f), m_Pitch(kStartPitch), m_MoveSpeed(kDefaultMoveSpeed) {
    SyncCamera();
}

void SpectatorCamera::SetView(const Frost::ViewPoint& view) {
    m_Position = view.position;
    m_Yaw = std::remainder(glm::radians(view.yawDegrees), glm::two_pi<float>());
    m_Pitch = std::clamp(glm::radians(view.pitchDegrees), -kMaxPitch, kMaxPitch);
    SyncCamera();
}

void SpectatorCamera::Frame(const Frost::Bounds& bounds) {
    // Back off along the view until a sphere around the box fills the (vertical) field of view.
    const float radius = std::max(glm::length(bounds.GetSize()) * 0.5f, 0.01f);
    const float distance = kFrameMargin * radius / std::sin(m_Camera.GetVerticalFov() * 0.5f);

    m_Position = bounds.GetCenter() - m_Camera.GetForward() * distance;
    SyncCamera();
}

void SpectatorCamera::OnUpdate(const float deltaTime, Frost::Input& input) {
    if (!m_HasMouse && input.WasMouseButtonPressed(Frost::MouseButton::Left)) {
        m_HasMouse = true;
        input.SetCursorMode(Frost::CursorMode::Captured);
    }
    else if (m_HasMouse && input.WasKeyPressed(Frost::Key::Escape)) {
        ReleaseMouse(input);
    }

    if (m_HasMouse) {
        // Screen +X is right, +Y down: moving the mouse right turns right, up looks up.
        const glm::vec2 mouseDelta = input.GetMouseDelta();
        m_Yaw = std::remainder(m_Yaw - mouseDelta.x * kLookSensitivity, glm::two_pi<float>());
        m_Pitch = std::clamp(m_Pitch - mouseDelta.y * kLookSensitivity, -kMaxPitch, kMaxPitch);

        if (const float scroll = input.GetScrollDelta().y; scroll != 0.0f) {
            m_MoveSpeed = std::clamp(m_MoveSpeed * std::pow(kSpeedStep, scroll), kMinMoveSpeed, kMaxMoveSpeed);
        }
        SyncCamera();
    }

    // Keys reach the game only while its window has focus, so moving needs no mouse.
    glm::vec3 direction(0.0f);
    if (input.IsKeyDown(Frost::Key::W)) { direction += m_Camera.GetForward(); }
    if (input.IsKeyDown(Frost::Key::S)) { direction -= m_Camera.GetForward(); }
    if (input.IsKeyDown(Frost::Key::D)) { direction += m_Camera.GetRight(); }
    if (input.IsKeyDown(Frost::Key::A)) { direction -= m_Camera.GetRight(); }
    if (input.IsKeyDown(Frost::Key::Space)) { direction += kWorldUp; }
    if (input.IsKeyDown(Frost::Key::LeftControl)) { direction -= kWorldUp; }

    if (glm::dot(direction, direction) > 1e-6f) {
        const float speed = m_MoveSpeed * (input.IsKeyDown(Frost::Key::LeftShift) ? kFastFactor : 1.0f);
        m_Position += glm::normalize(direction) * (speed * std::min(deltaTime, kMaxDeltaTime));
        SyncCamera();
    }
}

bool SpectatorCamera::HasMouse() const noexcept { return m_HasMouse; }

void SpectatorCamera::ReleaseMouse(Frost::Input& input) {
    if (!m_HasMouse) { return; }
    m_HasMouse = false;
    input.SetCursorMode(Frost::CursorMode::Normal);
}

const Frost::Camera& SpectatorCamera::GetCamera() const noexcept { return m_Camera; }

void SpectatorCamera::SyncCamera() {
    // Yaw about world up, then pitch about the camera's own right axis (as the editor camera does,
    // so an editor view passed in with --view looks the same here).
    const glm::quat yaw = glm::angleAxis(m_Yaw, kWorldUp);
    const glm::quat pitch = glm::angleAxis(m_Pitch, glm::vec3(1.0f, 0.0f, 0.0f));

    m_Camera.SetPosition(m_Position);
    m_Camera.SetOrientation(yaw * pitch);
}
