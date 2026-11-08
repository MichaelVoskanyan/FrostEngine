#include <Frost/Debug/DebugDraw.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

    // The 12 edges of a box, as pairs of corner indices (see OrientedBox::GetCorners) that differ in
    // exactly one axis bit.
    constexpr std::array<std::pair<int, int>, 12> kBoxEdges = {{{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                                               {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                                               {0, 4}, {1, 5}, {2, 6}, {3, 7}}};

    // Two unit vectors perpendicular to `axis` (unit length) and to each other.
    void MakePerpendiculars(const glm::vec3& axis, glm::vec3& first, glm::vec3& second) noexcept {
        const glm::vec3 helper = std::abs(axis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        first = glm::normalize(glm::cross(axis, helper));
        second = glm::cross(axis, first);
    }

    // `vector` at unit length, or nothing if it has no direction.
    [[nodiscard]]
    std::optional<glm::vec3> Direction(const glm::vec3& vector) noexcept {
        const float length = glm::length(vector);
        if (!(length > 1.0e-6f) || !std::isfinite(length)) { return std::nullopt; }
        return vector / length;
    }

    // A point of the screen for a view-space point in front of the camera.
    [[nodiscard]]
    glm::vec2 ViewToScreen(const glm::mat4& projection, const glm::vec2& screenSize, const glm::vec3& view) noexcept {
        const glm::vec4 clip = projection * glm::vec4(view, 1.0f);
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        // Clip space keeps +Y up (see Camera); the screen's y grows downwards.
        return glm::vec2((ndc.x * 0.5f + 0.5f) * screenSize.x, (0.5f - ndc.y * 0.5f) * screenSize.y);
    }

}  // namespace

namespace Frost {

    void DebugDraw::DrawLine(const glm::vec3& from, const glm::vec3& to, const DebugColor& color, const float thickness) {
        m_Lines.push_back(Line{.from = from, .to = to, .color = color, .thickness = thickness});
    }

    void DebugDraw::DrawArrow(const glm::vec3& from, const glm::vec3& to, const DebugColor& color, const float thickness) {
        DrawLine(from, to, color, thickness);

        const std::optional<glm::vec3> direction = Direction(to - from);
        if (!direction.has_value()) { return; }

        // A four-sided head, a fifth of the arrow long (at most 20 cm) and half as wide.
        const float length = std::min(glm::length(to - from) * 0.2f, 20.0f * kCentimeter);
        glm::vec3 side;
        glm::vec3 up;
        MakePerpendiculars(*direction, side, up);
        const glm::vec3 back = to - *direction * length;
        for (const glm::vec3& spread : {side, -side, up, -up}) { DrawLine(to, back + spread * (length * 0.5f), color, thickness); }
    }

    void DebugDraw::DrawPoint(const glm::vec3& position, const float size, const DebugColor& color) {
        const float half = size * 0.5f;
        DrawLine(position - glm::vec3(half, 0.0f, 0.0f), position + glm::vec3(half, 0.0f, 0.0f), color);
        DrawLine(position - glm::vec3(0.0f, half, 0.0f), position + glm::vec3(0.0f, half, 0.0f), color);
        DrawLine(position - glm::vec3(0.0f, 0.0f, half), position + glm::vec3(0.0f, 0.0f, half), color);
    }

    void DebugDraw::DrawCircle(const glm::vec3& center, const glm::vec3& normal, const float radius, const DebugColor& color) {
        const std::optional<glm::vec3> axis = Direction(normal);
        if (!axis.has_value() || !(radius > 0.0f)) { return; }

        glm::vec3 first;
        glm::vec3 second;
        MakePerpendiculars(*axis, first, second);
        glm::vec3 previous = center + first * radius;
        for (int i = 1; i <= kCircleSegments; ++i) {
            const float angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(kCircleSegments);
            const glm::vec3 point = center + (first * std::cos(angle) + second * std::sin(angle)) * radius;
            DrawLine(previous, point, color);
            previous = point;
        }
    }

    void DebugDraw::DrawSphere(const glm::vec3& center, const float radius, const DebugColor& color) {
        DrawCircle(center, glm::vec3(1.0f, 0.0f, 0.0f), radius, color);
        DrawCircle(center, glm::vec3(0.0f, 1.0f, 0.0f), radius, color);
        DrawCircle(center, glm::vec3(0.0f, 0.0f, 1.0f), radius, color);
    }

    void DebugDraw::DrawCapsule(const Capsule& capsule, const DebugColor& color) {
        const std::optional<glm::vec3> axis = Direction(capsule.end - capsule.start);
        if (!axis.has_value()) {
            DrawSphere(capsule.start, capsule.radius, color);  // A sphere.
            return;
        }
        if (!(capsule.radius > 0.0f)) {
            DrawLine(capsule.start, capsule.end, color);  // Just its core.
            return;
        }

        glm::vec3 first;
        glm::vec3 second;
        MakePerpendiculars(*axis, first, second);
        const float radius = capsule.radius;

        DrawCircle(capsule.start, *axis, radius, color);
        DrawCircle(capsule.end, *axis, radius, color);
        for (const glm::vec3& side : {first, -first, second, -second}) {
            DrawLine(capsule.start + side * radius, capsule.end + side * radius, color);
        }

        // The end caps: half circles over each end, in the two planes along the axis.
        constexpr int kHalfSegments = kCircleSegments / 2;
        for (const auto& [center, outward] : {std::pair{capsule.end, *axis}, std::pair{capsule.start, -*axis}}) {
            for (const glm::vec3& across : {first, second}) {
                glm::vec3 previous = center + across * radius;
                for (int i = 1; i <= kHalfSegments; ++i) {
                    const float angle = glm::pi<float>() * static_cast<float>(i) / static_cast<float>(kHalfSegments);
                    const glm::vec3 point = center + (across * std::cos(angle) + outward * std::sin(angle)) * radius;
                    DrawLine(previous, point, color);
                    previous = point;
                }
            }
        }
    }

    void DebugDraw::DrawBox(const OrientedBox& box, const DebugColor& color) {
        const std::array<glm::vec3, 8> corners = box.GetCorners();
        for (const auto& [first, second] : kBoxEdges) {
            DrawLine(corners[static_cast<std::size_t>(first)], corners[static_cast<std::size_t>(second)], color);
        }
    }

    void DebugDraw::DrawShape(const CollisionShape& shape, const DebugColor& color) {
        if (const OrientedBox* const box = std::get_if<OrientedBox>(&shape)) { DrawBox(*box, color); }
        else { DrawCapsule(std::get<Capsule>(shape), color); }
    }

    void DebugDraw::DrawLabel(const glm::vec3& position, std::string text, const DebugColor& color) {
        m_Labels.push_back(Label{.position = position, .text = std::move(text), .color = color});
    }

    void DebugDraw::Clear() noexcept {
        m_Lines.clear();
        m_Labels.clear();
    }

    bool DebugDraw::IsEmpty() const noexcept { return m_Lines.empty() && m_Labels.empty(); }

    const std::vector<DebugDraw::Line>& DebugDraw::GetLines() const noexcept { return m_Lines; }

    const std::vector<DebugDraw::Label>& DebugDraw::GetLabels() const noexcept { return m_Labels; }

    std::optional<glm::vec2> ProjectToScreen(const glm::mat4& view, const glm::mat4& projection, const float nearClip,
                                             const glm::vec2& screenSize, const glm::vec3& point) noexcept {
        const glm::vec3 inView = glm::vec3(view * glm::vec4(point, 1.0f));
        if (!(-inView.z >= nearClip)) { return std::nullopt; }  // The camera looks down -Z.
        return ViewToScreen(projection, screenSize, inView);
    }

    std::optional<ScreenLine> ProjectLineToScreen(const glm::mat4& view, const glm::mat4& projection, const float nearClip,
                                                  const glm::vec2& screenSize, const glm::vec3& from,
                                                  const glm::vec3& to) noexcept {
        glm::vec3 a = glm::vec3(view * glm::vec4(from, 1.0f));
        glm::vec3 b = glm::vec3(view * glm::vec4(to, 1.0f));
        const float depthA = -a.z;
        const float depthB = -b.z;

        if (!std::isfinite(depthA) || !std::isfinite(depthB)) { return std::nullopt; }  // A broken transform.

        // Cut off the part behind the near plane (projecting it would flip it through the camera).
        if (depthA < nearClip && depthB < nearClip) { return std::nullopt; }
        if (depthA < nearClip) { a += (b - a) * ((nearClip - depthA) / (depthB - depthA)); }
        else if (depthB < nearClip) { b += (a - b) * ((nearClip - depthB) / (depthA - depthB)); }

        return ScreenLine{.from = ViewToScreen(projection, screenSize, a), .to = ViewToScreen(projection, screenSize, b)};
    }

}  // namespace Frost
