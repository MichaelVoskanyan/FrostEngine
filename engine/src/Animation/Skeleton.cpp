#include <Frost/Animation/Skeleton.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace {

    [[nodiscard]]
    bool IsFinite(const glm::vec3& v) noexcept {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    [[nodiscard]]
    bool IsFinite(const glm::quat& q) noexcept {
        return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
    }

}  // namespace

namespace Frost {

    glm::mat4 BoneTransform::ToMatrix() const noexcept {
        const glm::mat4 t = glm::translate(glm::mat4(1.0f), translation);
        const glm::mat4 r = glm::mat4_cast(rotation);
        const glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
        return t * r * s;
    }

    BoneTransform BoneTransform::FromMatrix(const glm::mat4& matrix) noexcept {
        BoneTransform result;
        result.translation = glm::vec3(matrix[3]);

        glm::vec3 columns[3] = {glm::vec3(matrix[0]), glm::vec3(matrix[1]), glm::vec3(matrix[2])};
        result.scale = glm::vec3(glm::length(columns[0]), glm::length(columns[1]), glm::length(columns[2]));

        // A mirroring matrix has a negative determinant: put the reflection into one scale axis so
        // what remains is a proper rotation.
        if (glm::determinant(glm::mat3(matrix)) < 0.0f) { result.scale.x = -result.scale.x; }

        if (result.scale.x == 0.0f || result.scale.y == 0.0f || result.scale.z == 0.0f) {
            return result;  // Collapsed: no rotation can be recovered; keep the identity.
        }

        for (int axis = 0; axis < 3; ++axis) { columns[axis] /= result.scale[axis]; }
        const glm::quat rotation = glm::quat_cast(glm::mat3(columns[0], columns[1], columns[2]));
        const float length = glm::length(rotation);
        if (length > 0.0f && std::isfinite(length)) { result.rotation = rotation / length; }
        return result;
    }

    std::optional<std::size_t> SkeletonData::FindBone(const std::string_view name) const noexcept {
        for (std::size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].name == name) { return i; }
        }
        return std::nullopt;
    }

    std::vector<BoneTransform> SkeletonData::GetRestPose() const {
        std::vector<BoneTransform> pose;
        pose.reserve(bones.size());
        for (const Bone& bone : bones) { pose.push_back(bone.restPose); }
        return pose;
    }

    void SkeletonData::ComputeModelTransforms(const std::span<const BoneTransform> localPose,
                                              const std::span<glm::mat4> modelTransforms) const {
        if (localPose.size() != bones.size() || modelTransforms.size() != bones.size()) {
            throw std::invalid_argument("SkeletonData::ComputeModelTransforms: one transform per bone is required.");
        }

        for (std::size_t i = 0; i < bones.size(); ++i) {
            const glm::mat4 local = localPose[i].ToMatrix();
            const std::int32_t parent = bones[i].parent;

            // Parents come first (see Validate), so a parent's transform is always final by now; a
            // malformed parent index is treated as a root rather than read out of bounds.
            modelTransforms[i] = parent >= 0 && static_cast<std::size_t>(parent) < i
                                         ? modelTransforms[static_cast<std::size_t>(parent)] * local
                                         : local;
        }
    }

    std::vector<glm::mat4> SkeletonData::ComputeModelTransforms(const std::span<const BoneTransform> localPose) const {
        std::vector<glm::mat4> transforms(bones.size(), glm::mat4(1.0f));
        ComputeModelTransforms(localPose, transforms);
        return transforms;
    }

    std::optional<std::string> SkeletonData::Validate() const {
        if (bones.empty()) { return "it has no bones"; }
        if (bones.size() > kMaxBones) { return "it has more than " + std::to_string(kMaxBones) + " bones"; }

        std::unordered_set<std::string_view> names;
        for (std::size_t i = 0; i < bones.size(); ++i) {
            const Bone& bone = bones[i];
            if (bone.parent < -1 || (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) >= i)) {
                return "bone " + std::to_string(i) + " has a parent that is not an earlier bone";
            }
            if (bone.name.empty()) { return "bone " + std::to_string(i) + " has no name"; }
            if (!names.insert(bone.name).second) { return "two bones are called '" + bone.name + "'"; }
            if (!IsFinite(bone.restPose.translation) || !IsFinite(bone.restPose.rotation) ||
                !IsFinite(bone.restPose.scale)) {
                return "bone '" + bone.name + "' has a non-finite rest pose";
            }
        }
        return std::nullopt;
    }

}  // namespace Frost
