#pragma once

#include <Frost/Animation/Skeleton.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Renderer/Vertex.hpp>

#include <glm/mat4x4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Frost {

    // Which bones move a vertex, and how much: up to four influences, each an index into the
    // skeletal mesh's bind table (SkeletalMeshData::bindBones) and a weight. Weights sum to 1 for a
    // skinned vertex; a vertex whose weights are all 0 is not skinned and stays where it is in the
    // mesh's space whatever the pose.
    struct SkinWeights {
        static constexpr std::size_t kMaxInfluences = 4;

        std::array<std::uint16_t, kMaxInfluences> bones{};
        std::array<float, kMaxInfluences> weights{};
    };

    // A mesh bound to a skeleton: geometry in its bind pose plus, per vertex, the bones that deform
    // it. Produced by GltfModel::ImportSkeletalMesh, stored as a .fskmesh (see AssetFile).
    //
    // The skin refers to bones through a bind table of its own rather than by skeleton index: each
    // entry names a bone and gives the inverse of that bone's model-space transform in the pose the
    // mesh was modeled in (its inverse bind matrix). A bone's skinning matrix is then its current
    // model-space transform times its inverse bind matrix (identity when the skeleton is in the
    // bind pose), and the table is matched to the skeleton by name when the mesh is loaded, so the
    // mesh keeps working when its skeleton is reimported with its bones reordered.
    //
    // A sub-mesh can instead be *attached* to a bone as a whole: a rigid part (a magazine, a muzzle
    // brake, a prop in a hand) that moves with that bone without being deformed, and that an object
    // can also leave where it was modeled (see SkeletalMeshComponent's attachments toggle). Its
    // vertices have no skin weights; they are stored so that the bone's skinning matrix takes them
    // to where the part belongs (in the bind pose, that is where it was modeled).
    struct SkeletalMeshData {
        // Bind-pose vertices (in world units, centimeters), indices and sub-meshes; importer output
        // also carries materials and embedded textures (see MeshData).
        MeshData mesh;
        std::vector<SkinWeights> skin;  // One per vertex of `mesh`.
        std::vector<std::string> bindBones;
        std::vector<glm::mat4> inverseBindMatrices;  // One per bindBones entry.
        // Per sub-mesh, the bind table entry of the bone it is attached to, or -1 (skinned, or not
        // moving with the skeleton at all). Empty = no sub-mesh is attached.
        std::vector<std::int32_t> attachments;
    };

    // For each bind table entry, the index of the bone of that name in `skeleton`, or -1 if it has
    // none.
    [[nodiscard]]
    std::vector<std::int32_t> BindSkin(std::span<const std::string> bindBones, const SkeletonData& skeleton);

    // The skinning matrix of every bind table entry: the bound bone's model-space transform times
    // the entry's inverse bind matrix. An entry whose bone is missing (binding -1) gets the
    // identity, leaving what it moves in its bind pose.
    [[nodiscard]]
    std::vector<glm::mat4> ComputeSkinMatrices(std::span<const std::int32_t> bindings,
                                               std::span<const glm::mat4> inverseBindMatrices,
                                               std::span<const glm::mat4> boneModelTransforms);

    // Deforms bind-pose vertices by the skin (linear blend skinning): each position is the weighted
    // blend of its bones' skinning matrices applied to it; normals and tangents follow (normals
    // through the blended matrix's inverse-transpose), renormalized. Vertices without weights are
    // copied unchanged. `out` is resized to match. Throws std::invalid_argument if the skin does
    // not have one entry per vertex, or refers to a matrix that does not exist.
    void SkinVertices(std::span<const Vertex> bindVertices, std::span<const SkinWeights> skin,
                      std::span<const glm::mat4> skinMatrices, std::vector<Vertex>& out);

    // As SkinVertices, positions only (for picking and bounds).
    void SkinPositions(std::span<const Vertex> bindVertices, std::span<const SkinWeights> skin,
                       std::span<const glm::mat4> skinMatrices, std::vector<glm::vec3>& out);

    // Why this is not a usable skeletal mesh (skin and vertex counts differ, an influence refers past
    // the bind table, a weight is negative or not finite, the bind table's names and matrices differ
    // in count or a name is empty, attachments are not one per sub-mesh or refer past the bind
    // table), or nullopt if it is one.
    [[nodiscard]]
    std::optional<std::string> ValidateSkin(const SkeletalMeshData& data);

}  // namespace Frost
