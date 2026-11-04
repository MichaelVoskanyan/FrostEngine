#include "EditorCamera.hpp"

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

    // Up and back from the origin, looking slightly down at it: a sensible view of an empty level
    // and of whatever gets placed near the middle of one.
    constexpr glm::vec3 kStartPosition{0.0f, 2.0f, 6.0f};
    constexpr float kStartPitch = glm::radians(-15.0f);

    // Focus leaves this much room around the framed sphere.
    constexpr float kFocusMargin = 1.25f;

    // Radians of rotation per screen unit of mouse movement (~0.11 degrees).
    constexpr float kLookSensitivity = 0.002f;

    // Just short of straight up/down: at exactly +-90 degrees yaw and "up" stop being meaningful.
    constexpr float kMaxPitch = glm::radians(89.0f);

    // World units per second. One wheel notch scales the speed by kSpeedStep (up = faster);
    // multiplicative steps keep the wheel equally useful for creeping up to something and for
    // crossing a large level.
    constexpr float kDefaultMoveSpeed = 2.0f;
    constexpr float kMinMoveSpeed = 0.05f;
    constexpr float kMaxMoveSpeed = 500.0f;
    constexpr float kSpeedStep = 1.2f;

    // A long frame (a hitch, a breakpoint, the window being dragged) is capped so a held key does
    // not teleport the camera.
    constexpr float kMaxDeltaTime = 0.1f;

    constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

}  // namespace

EditorCamera::EditorCamera() : m_Position(kStartPosition), m_Pitch(kStartPitch), m_MoveSpeed(kDefaultMoveSpeed) {
    SyncCamera();
}

void EditorCamera::Focus(const glm::vec3& center, const float radius) {
    if (m_Controlling) { return; }

    // Distance at which the sphere's silhouette spans the vertical field of view (the narrower one
    // for landscape viewports), plus a margin.
    const float safeRadius = std::max(radius, 0.01f);
    const float distance = kFocusMargin * safeRadius / std::sin(m_Camera.GetVerticalFov() * 0.5f);

    m_Position = center - m_Camera.GetForward() * distance;
    SyncCamera();
}

void EditorCamera::OnUpdate(const float deltaTime, Frost::Input& input, const bool allowBeginControl) {
    // Control starts only on a fresh press that is ours to take (not one that began over a UI panel
    // and was then dragged into the viewport), and ends the moment the button comes up.
    if (!m_Controlling) {
        if (allowBeginControl && input.WasMouseButtonPressed(Frost::MouseButton::Right)) { BeginControl(input); }
    }
    else if (!input.IsMouseButtonDown(Frost::MouseButton::Right)) {
        EndControl(input);
    }

    if (!m_Controlling) { return; }

    // Look. Screen +X is right and +Y is down; moving the mouse right turns right (negative yaw),
    // moving it up looks up.
    const glm::vec2 mouseDelta = input.GetMouseDelta();
    m_Yaw -= mouseDelta.x * kLookSensitivity;
    m_Pitch = std::clamp(m_Pitch - mouseDelta.y * kLookSensitivity, -kMaxPitch, kMaxPitch);

    // Keep yaw in [-pi, pi) so it does not slowly lose precision over a long session of spinning.
    m_Yaw = std::remainder(m_Yaw, glm::two_pi<float>());

    // Speed. Only while controlling: otherwise the wheel belongs to the UI (scrolling panels).
    if (const float scroll = input.GetScrollDelta().y; scroll != 0.0f) {
        m_MoveSpeed = std::clamp(m_MoveSpeed * std::pow(kSpeedStep, scroll), kMinMoveSpeed, kMaxMoveSpeed);
    }

    // Move, using the orientation just updated.
    SyncCamera();

    glm::vec3 direction(0.0f);
    if (input.IsKeyDown(Frost::Key::W)) { direction += m_Camera.GetForward(); }
    if (input.IsKeyDown(Frost::Key::S)) { direction -= m_Camera.GetForward(); }
    if (input.IsKeyDown(Frost::Key::D)) { direction += m_Camera.GetRight(); }
    if (input.IsKeyDown(Frost::Key::A)) { direction -= m_Camera.GetRight(); }
    if (input.IsKeyDown(Frost::Key::Space)) { direction += kWorldUp; }
    if (input.IsKeyDown(Frost::Key::LeftControl)) { direction -= kWorldUp; }

    // Normalized so diagonals are no faster than a single axis; opposing keys cancel out.
    if (glm::dot(direction, direction) > 1e-6f) {
        const float step = m_MoveSpeed * std::min(deltaTime, kMaxDeltaTime);
        m_Position += glm::normalize(direction) * step;
        SyncCamera();
    }
}

bool EditorCamera::IsControlling() const noexcept { return m_Controlling; }

const Frost::Camera& EditorCamera::GetCamera() const noexcept { return m_Camera; }

const glm::vec3& EditorCamera::GetPosition() const noexcept { return m_Position; }

float EditorCamera::GetYawDegrees() const noexcept { return glm::degrees(m_Yaw); }

float EditorCamera::GetPitchDegrees() const noexcept { return glm::degrees(m_Pitch); }

float EditorCamera::GetMoveSpeed() const noexcept { return m_MoveSpeed; }

void EditorCamera::BeginControl(Frost::Input& input) {
    m_Controlling = true;
    input.SetCursorMode(Frost::CursorMode::Captured);
}

void EditorCamera::EndControl(Frost::Input& input) {
    m_Controlling = false;
    input.SetCursorMode(Frost::CursorMode::Normal);
}

void EditorCamera::SyncCamera() {
    // Yaw about world up, then pitch about the camera's own right axis.
    const glm::quat yaw = glm::angleAxis(m_Yaw, kWorldUp);
    const glm::quat pitch = glm::angleAxis(m_Pitch, glm::vec3(1.0f, 0.0f, 0.0f));

    m_Camera.SetPosition(m_Position);
    m_Camera.SetOrientation(yaw * pitch);
}
