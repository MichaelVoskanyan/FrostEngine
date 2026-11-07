#include <Frost/Animation/SkinnedMesh.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <cmath>
#include <stdexcept>

namespace {

    [[nodiscard]]
    glm::vec3 SafeNormalize(const glm::vec3& v) noexcept {
        const float lengthSquared = glm::dot(v, v);
        return lengthSquared > 0.0f ? v / std::sqrt(lengthSquared) : v;
    }

}  // namespace

namespace Frost {

    std::vector<std::int32_t> BindSkin(const std::span<const std::string> bindBones, const SkeletonData& skeleton) {
        std::vector<std::int32_t> bindings;
        bindings.reserve(bindBones.size());
        for (const std::string& name : bindBones) {
            const std::optional<std::size_t> bone = skeleton.FindBone(name);
            bindings.push_back(bone.has_value() ? static_cast<std::int32_t>(*bone) : -1);
        }
        return bindings;
    }

    std::vector<glm::mat4> ComputeSkinMatrices(const std::span<const std::int32_t> bindings,
                                               const std::span<const glm::mat4> inverseBindMatrices,
                                               const std::span<const glm::mat4> boneModelTransforms) {
        std::vector<glm::mat4> matrices(inverseBindMatrices.size(), glm::mat4(1.0f));
        for (std::size_t i = 0; i < matrices.size() && i < bindings.size(); ++i) {
            const std::int32_t bone = bindings[i];
            if (bone >= 0 && static_cast<std::size_t>(bone) < boneModelTransforms.size()) {
                matrices[i] = boneModelTransforms[static_cast<std::size_t>(bone)] * inverseBindMatrices[i];
            }
        }
        return matrices;
    }

    void SkinVertices(const std::span<const Vertex> bindVertices, const std::span<const SkinWeights> skin,
                      const std::span<const glm::mat4> skinMatrices, std::vector<Vertex>& out) {
        if (skin.size() != bindVertices.size()) {
            throw std::invalid_argument("SkinVertices: the skin needs one entry per vertex.");
        }

        out.resize(bindVertices.size());
        for (std::size_t i = 0; i < bindVertices.size(); ++i) {
            const Vertex& source = bindVertices[i];
            const SkinWeights& weights = skin[i];
            Vertex& target = out[i];
            target = source;

            glm::mat4 blended(0.0f);
            float total = 0.0f;
            for (std::size_t influence = 0; influence < SkinWeights::kMaxInfluences; ++influence) {
                const float weight = weights.weights[influence];
                if (weight == 0.0f) { continue; }

                const std::uint16_t bone = weights.bones[influence];
                if (bone >= skinMatrices.size()) {
                    throw std::invalid_argument("SkinVertices: a vertex refers to a bone the skin has no matrix for.");
                }
                blended += skinMatrices[bone] * weight;
                total += weight;
            }
            if (total == 0.0f) { continue; }  // Not skinned: stays in the mesh's space.

            const glm::mat3 linear(blended);
            const float determinant = glm::determinant(linear);
            const glm::mat3 normalMatrix = determinant != 0.0f ? glm::inverseTranspose(linear) : linear;

            target.position = glm::vec3(blended * glm::vec4(source.position, 1.0f));
            target.normal = SafeNormalize(normalMatrix * source.normal);
            target.tangent = glm::vec4(SafeNormalize(linear * glm::vec3(source.tangent)), source.tangent.w);
        }
    }

    void SkinPositions(const std::span<const Vertex> bindVertices, const std::span<const SkinWeights> skin,
                       const std::span<const glm::mat4> skinMatrices, std::vector<glm::vec3>& out) {
        if (skin.size() != bindVertices.size()) {
            throw std::invalid_argument("SkinPositions: the skin needs one entry per vertex.");
        }

        out.resize(bindVertices.size());
        for (std::size_t i = 0; i < bindVertices.size(); ++i) {
            const glm::vec4 position(bindVertices[i].position, 1.0f);
            glm::vec3 skinned(0.0f);
            float total = 0.0f;
            for (std::size_t influence = 0; influence < SkinWeights::kMaxInfluences; ++influence) {
                const float weight = skin[i].weights[influence];
                if (weight == 0.0f) { continue; }

                const std::uint16_t bone = skin[i].bones[influence];
                if (bone >= skinMatrices.size()) {
                    throw std::invalid_argument("SkinPositions: a vertex refers to a bone the skin has no matrix for.");
                }
                skinned += glm::vec3(skinMatrices[bone] * position) * weight;
                total += weight;
            }
            out[i] = total == 0.0f ? bindVertices[i].position : skinned;
        }
    }

    std::optional<std::string> ValidateSkin(const SkeletalMeshData& data) {
        if (data.skin.size() != data.mesh.vertices.size()) { return "its skin does not have one entry per vertex"; }
        if (data.bindBones.size() != data.inverseBindMatrices.size()) {
            return "its bind table has a different number of bones and matrices";
        }
        // Influences index the table with 16 bits.
        if (data.bindBones.size() > std::size_t{65536}) { return "its bind table has too many bones"; }

        for (const std::string& name : data.bindBones) {
            if (name.empty()) { return "its bind table has a bone without a name"; }
        }
        for (const glm::mat4& matrix : data.inverseBindMatrices) {
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    if (!std::isfinite(matrix[column][row])) { return "its bind table has a non-finite matrix"; }
                }
            }
        }

        for (const SkinWeights& weights : data.skin) {
            for (std::size_t influence = 0; influence < SkinWeights::kMaxInfluences; ++influence) {
                const float weight = weights.weights[influence];
                if (!std::isfinite(weight) || weight < 0.0f) { return "its skin has a negative or non-finite weight"; }
                if (weight > 0.0f && weights.bones[influence] >= data.bindBones.size()) {
                    return "its skin refers to a bone past the end of its bind table";
                }
            }
        }

        if (!data.attachments.empty()) {
            const std::size_t subMeshCount = data.mesh.subMeshes.empty() ? 1 : data.mesh.subMeshes.size();
            if (data.attachments.size() != subMeshCount) { return "it does not have one attachment entry per sub-mesh"; }
            for (const std::int32_t attachment : data.attachments) {
                if (attachment < -1 || (attachment >= 0 && static_cast<std::size_t>(attachment) >= data.bindBones.size())) {
                    return "a sub-mesh is attached to a bone past the end of its bind table";
                }
            }
        }
        return std::nullopt;
    }

}  // namespace Frost
