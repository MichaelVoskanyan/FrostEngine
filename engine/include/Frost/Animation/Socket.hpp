#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <optional>
#include <span>
#include <string>

namespace Frost {

    // A named point on a skeletal mesh that objects can be attached to (see AttachmentComponent):
    // a muzzle, a sight rail, a hand grip. It sits at an offset from a bone, moving with it, or from
    // the skeletal mesh itself. Authored in the editor's skeletal mesh editor; stored in the
    // skeletal mesh asset (see SkeletalMeshFile::sockets).
    struct SocketData {
        // Unique among the skeletal mesh's sockets, and not the name of one of its skeleton's bones.
        std::string name;
        // The bone it moves with; empty = the skeletal mesh's own space (it moves with the object).
        std::string bone;

        // Its offset from the bone (or the mesh), composed translate * rotate * scale: position in
        // world units (centimeters) along the bone's axes, rotation as Euler angles in degrees in
        // TransformComponent's convention (x = pitch, y = yaw, z = roll; R = Ry * Rx * Rz). The
        // bone's own scale does not apply (see ComputeSocketMatrix): a rig scaled down in its
        // armature still has sockets offset in true centimeters, and what is attached to them keeps
        // its size.
        glm::vec3 position{0.0f};
        glm::vec3 rotation{0.0f};
        glm::vec3 scale{1.0f};

        // The offset as a matrix: from the socket's space to the bone's (or the mesh's).
        [[nodiscard]]
        glm::mat4 GetLocalMatrix() const noexcept;
    };

    // Where a socket is in the model's space, given its bone's model-space transform (identity for a
    // socket on the mesh itself): the bone's position and orientation, without its scale, then the
    // socket's offset.
    [[nodiscard]]
    glm::mat4 ComputeSocketMatrix(const glm::mat4& boneModelTransform, const SocketData& socket) noexcept;

    // Why these are not usable sockets (an empty or duplicate name, a non-finite offset), or nullopt.
    [[nodiscard]]
    std::optional<std::string> ValidateSockets(std::span<const SocketData> sockets);

}  // namespace Frost
