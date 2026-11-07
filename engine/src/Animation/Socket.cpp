#include <Frost/Animation/Socket.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <set>

namespace Frost {

    glm::mat4 SocketData::GetLocalMatrix() const noexcept {
        const glm::quat yaw = glm::angleAxis(glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::quat pitch = glm::angleAxis(glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::quat roll = glm::angleAxis(glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(yaw * pitch * roll) * glm::scale(glm::mat4(1.0f), scale);
    }

    glm::mat4 ComputeSocketMatrix(const glm::mat4& boneModelTransform, const SocketData& socket) noexcept {
        glm::mat4 frame = boneModelTransform;
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            const float length = glm::length(glm::vec3(frame[axis]));
            if (length > 0.0f) { frame[axis] /= length; }
        }
        if (glm::determinant(glm::mat3(frame)) < 0.0f) { frame[0] = -frame[0]; }  // A mirroring scale: keep a rotation.
        return frame * socket.GetLocalMatrix();
    }

    std::optional<std::string> ValidateSockets(const std::span<const SocketData> sockets) {
        const auto finite = [](const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };

        std::set<std::string> names;
        for (const SocketData& socket : sockets) {
            if (socket.name.empty()) { return "a socket has no name"; }
            if (!names.insert(socket.name).second) { return "two sockets are called '" + socket.name + "'"; }
            if (!finite(socket.position) || !finite(socket.rotation) || !finite(socket.scale)) {
                return "socket '" + socket.name + "' has a non-finite offset";
            }
        }
        return std::nullopt;
    }

}  // namespace Frost
