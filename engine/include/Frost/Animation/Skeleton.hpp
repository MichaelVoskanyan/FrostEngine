#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Frost {

    // A quaternion from its components in x, y, z, w order (glTF's), whatever glm's constructor
    // argument order is configured to be (it depends on GLM_FORCE_QUAT_DATA_XYZW).
    [[nodiscard]]
    inline glm::quat MakeQuaternion(const float x, const float y, const float z, const float w) noexcept {
        glm::quat quaternion;
        quaternion.x = x;
        quaternion.y = y;
        quaternion.z = z;
        quaternion.w = w;
        return quaternion;
    }

    [[nodiscard]]
    inline glm::quat IdentityQuaternion() noexcept {
        return MakeQuaternion(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // A bone's transform relative to its parent bone (or, for a root bone, to the model's space):
    // translation, rotation and scale, composed T * R * S as in glTF. Translations are in world units
    // (centimeters, see Frost/Units.hpp).
    struct BoneTransform {
        glm::vec3 translation{0.0f};
        glm::quat rotation = IdentityQuaternion();  // Unit length.
        glm::vec3 scale{1.0f};

        [[nodiscard]]
        glm::mat4 ToMatrix() const noexcept;

        // Decomposes an affine matrix into translation, rotation and (possibly negative) scale.
        // Shear cannot be represented and is lost; a singular matrix gives a zero scale.
        [[nodiscard]]
        static BoneTransform FromMatrix(const glm::mat4& matrix) noexcept;
    };

    struct Bone {
        // Unique within its skeleton: animations and skeletal meshes refer to bones by name, so they
        // keep working when the skeleton is reimported with its bones in a different order.
        std::string name;
        // Index of the parent bone (always lower than this bone's own index), or -1 for a root.
        std::int32_t parent = -1;
        // The bone's local transform in the skeleton's reference ("rest") pose.
        BoneTransform restPose;
        // Added in the editor rather than imported (e.g. a bone to hang a prop from): a reimport of
        // the skeleton keeps it.
        bool addedInEditor = false;
    };

    // A bone hierarchy: what skeletal meshes are bound to and animations move. Bones are stored
    // parents first, so a pose can be accumulated from the root down in a single pass. A skeleton may
    // have several roots.
    struct SkeletonData {
        // Bone indices are stored as 16-bit values in skeletal meshes.
        static constexpr std::size_t kMaxBones = 65535;

        std::vector<Bone> bones;

        // The index of the bone with this name, if any.
        [[nodiscard]]
        std::optional<std::size_t> FindBone(std::string_view name) const noexcept;

        // Every bone's rest-pose local transform, by bone index.
        [[nodiscard]]
        std::vector<BoneTransform> GetRestPose() const;

        // Each bone's transform in the model's space for a pose of local transforms (one per bone,
        // by index): its own local transform on top of its parent's model-space transform. Both
        // spans must have one element per bone.
        void ComputeModelTransforms(std::span<const BoneTransform> localPose, std::span<glm::mat4> modelTransforms) const;

        // As above, into a new vector.
        [[nodiscard]]
        std::vector<glm::mat4> ComputeModelTransforms(std::span<const BoneTransform> localPose) const;

        // Why this is not a usable skeleton (no bones, too many, a parent that is not an earlier
        // bone, an empty or duplicate name, a non-finite transform), or nullopt if it is one.
        [[nodiscard]]
        std::optional<std::string> Validate() const;
    };

}  // namespace Frost
