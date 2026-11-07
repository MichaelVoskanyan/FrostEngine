#pragma once

#include <Frost/Animation/AnimationClip.hpp>
#include <Frost/Animation/Skeleton.hpp>
#include <Frost/Animation/SkinnedMesh.hpp>
#include <Frost/Assets/ImportError.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Frost {

    // One of a glTF file's animations, as GltfModel::GetInfo describes it.
    struct GltfAnimationInfo {
        std::string name;      // Its name, or "Animation <index>" if it has none.
        bool hasName = false;  // Whether `name` is the file's (rather than made up).
        float duration = 0.0f;  // Seconds: its last keyframe.
        // Channels that move nodes (translation/rotation/scale; morph target weights are not counted).
        std::size_t channelCount = 0;
        std::string problem;  // Why it cannot be imported (malformed), or empty.
    };

    // A rigid part of a model (a mesh that is not skinned), as a skeletal mesh import sees it.
    struct GltfPartInfo {
        std::string name;        // The part (sub-mesh) name: its node's name, else its mesh's.
        std::string parentBone;  // The bone at or above its node in the file's hierarchy; empty for a loose part.
        // For a loose part, the bone it is attached to by default (see GltfAttachmentOptions), or
        // empty if there is none to suggest.
        std::string suggestedBone;
    };

    // How a skeletal mesh import attaches the model's rigid parts to bones. A part under a bone in
    // the file's hierarchy is attached to that bone. A *loose* part (no bone above it, e.g. a
    // magazine placed beside the armature rather than parented to a bone, as exporters leave parts
    // that were attached with constraints) is attached to a suggested bone: a skin joint or animated
    // bone whose name shares a word with the part's (preferring bones that deform nothing, as those
    // exist to carry parts: "AK12Mag" -> "MagEquipped" over "MagCatch"), else the skeleton's root
    // joint.
    struct GltfAttachmentOptions {
        // Off: loose parts do not move with the skeleton.
        bool attachLooseParts = true;
        // Per part (by name), the bone to attach it to instead, whatever the above; "" = none.
        std::map<std::string, std::string> partBones;
        // Parts (by name) left out of the skeletal mesh and imported as meshes of their own, to be
        // attached to it as separate objects (see GltfSeparatePart), e.g. a magazine or muzzle
        // device that a level may swap for another. Only for imports that ask for them.
        std::set<std::string> separateParts;
    };

    // A part a skeletal mesh import left out to be imported as a mesh of its own (see
    // GltfAttachmentOptions::separateParts). Its geometry is where the file has it, in the skeletal
    // mesh's space (centimeters): attached to the skeletal mesh's object with no offset, moving
    // with `bone` relative to the bone's rest pose (see AttachmentComponent), it sits where it was
    // modeled.
    struct GltfSeparatePart {
        std::string name;  // The part's name (see GltfPartInfo).
        // The bone it moves with: the one chosen for it, else the one above it in the file, else a
        // suggested one (as for attached parts); empty = the skeletal mesh's object itself.
        std::string bone;
        // Its geometry. Its sub-meshes' materialIndex indexes the skeletal mesh's materials (which
        // include the parts'); its own materials and textures lists are empty.
        MeshData mesh;
    };

    // What a glTF file contains, as far as importing it is concerned.
    struct GltfModelInfo {
        std::size_t meshCount = 0;  // Nodes of the default scene that have a mesh.
        std::size_t skinCount = 0;
        // The skeleton ImportSkeleton makes: its bone count, and whether it comes from the file's
        // skins (true) or, in a file without skins, from its node hierarchy (false).
        std::size_t boneCount = 0;
        bool skinned = false;
        std::string skeletonProblem;  // Why no skeleton can be made (malformed skins), or empty.
        std::vector<std::string> boneNames;  // The skeleton's bones, in order.
        // The rigid parts a skeletal mesh import attaches to bones, one per part name.
        std::vector<GltfPartInfo> parts;
        std::vector<GltfAnimationInfo> animations;
    };

    // A parsed glTF 2.0 file, from which any of the assets it can become are imported: a static
    // mesh, a skeleton, a skeletal mesh, and each of its animations. Both containers are accepted:
    // binary .glb (JSON + BIN chunk in one file) and JSON .gltf, whose buffers and images are
    // fetched from the URIs it lists (base64 data: URIs, or files relative to the .gltf such as its
    // .bin). A broken link (missing or truncated external file) is reported as an ImportError naming
    // the file, never a crash. The file is read once; its external buffers are loaded on first use.
    //
    // glTF is in meters and the world in centimeters (Frost/Units.hpp): every position, translation
    // and length imported is converted.
    //
    // Static meshes (ImportMesh): the default scene (or the first one, or every root node if the
    // file declares no scenes) is walked depth-first. Every mesh primitive reached becomes one
    // SubMesh: its vertices are converted to the engine's Vertex layout and transformed by the
    // instancing node's world matrix (the product of the node chain's local matrix /
    // translation-rotation-scale transforms), so the output is the model as posed in the file, in a
    // single object space. A skinned mesh is posed by its skin instead (its joints' world
    // transforms times their inverse bind matrices, blended by the vertex weights), since the
    // specification says the transform of a skinned mesh's own node does not apply to it. Normals
    // are transformed by the inverse-transpose and renormalized; a mirroring transform (negative
    // determinant) has its triangle winding flipped so front faces stay counter-clockwise. A node
    // with several mesh references, or a mesh instanced by several nodes, yields one SubMesh per
    // instance.
    //
    // Geometry: TRIANGLES, TRIANGLE_STRIP and TRIANGLE_FAN primitives (strips/fans are expanded to
    // lists), indexed and non-indexed geometry, every accessor component type (including the
    // normalized integer formats of KHR_mesh_quantization), interleaved buffer views (byteStride),
    // POSITION, NORMAL, TEXCOORD_0 and COLOR_0 (vec3 or vec4) attributes, and for skinning
    // JOINTS_n/WEIGHTS_n (every set; the four strongest influences per vertex are kept and
    // renormalized). Missing normals are generated flat, per the glTF specification (the primitive
    // is de-indexed to do so). Missing texture coordinates read as (0, 0); missing colors read as
    // opaque white. Tangents (for normal mapping) are taken from the TANGENT attribute when present
    // and otherwise derived from the UV layout, so every textured primitive gets a tangent frame.
    //
    // Materials: each primitive's material is imported into MeshData::materials (one per glTF
    // material, referenced by the SubMesh's materialIndex; -1 for primitives without one): the
    // metallic-roughness factors, base color, metallic-roughness, normal (with scale), occlusion
    // (with strength) and emissive textures, and the emissive factor (including
    // KHR_materials_emissive_strength). Textures (PNG or JPEG; embedded in the BIN chunk, in a
    // base64 data: URI, or in a file next to the model) are decoded into MeshData::textures with
    // their sampler settings and the color space their slot implies (base color and emissive are
    // sRGB, the rest linear data); a texture shared by several materials is decoded once. Textures
    // mapped through a UV set other than TEXCOORD_0, or whose only image source is an extension
    // format, are left off rather than misapplied. Alpha mode and double-sidedness are ignored.
    //
    // Skeletons (ImportSkeleton): from the file's skins, every joint of every skin and every node
    // an animation moves (a prop in a hand, a weapon's magazine), plus every node above those (so
    // the file's placement of the skeleton, e.g. under an "Armature" node, is part of it); in a file
    // without skins, every node of the default scene, so models whose parts are animated as a node
    // hierarchy (a weapon's slide, a door) can be imported with their animations too. Bones are named after their nodes (unnamed nodes are "Node<index>"; a
    // repeated name gets "_<index>" appended, so names are unique) and keep their nodes' local
    // transforms as the rest pose.
    //
    // Skeletal meshes (ImportSkeletalMesh): the same geometry as a static import, bound to a
    // skeleton by bone name: a skinned mesh by its skin (its vertices stay in the skin's bind
    // space, with the skin's inverse bind matrices), any other mesh attached as a whole to a bone
    // (see GltfAttachmentOptions): by default the bone above it in the file (a prop parented to a
    // hand bone moves with the hand), or for a loose part a suggested one.
    //
    // Animations (ImportAnimation): one clip per glTF animation; every translation, rotation and
    // scale channel becomes part of the track of the bone named after its node (STEP, LINEAR and
    // CUBICSPLINE interpolation are kept as they are). Morph target weight channels are not
    // supported and are left out (with a warning).
    //
    // Not supported (throws ImportError): absolute or remote (http:, ...) URIs, sparse accessors,
    // and Draco / meshopt compressed geometry. Point and line primitives, morph targets and cameras
    // are ignored.
    //
    // Sub-meshes are named after their node (or mesh, for unnamed nodes), so a specific part of a
    // model can be found with Mesh::FindSubMesh, e.g. to apply a texture imported separately.
    class GltfModel final {
    public:
        // Reads and parses the file. Throws ImportError if it cannot be read, or is not valid
        // glTF 2.0 (.glb or .gltf) or requires an unsupported extension.
        [[nodiscard]]
        static GltfModel Open(const std::filesystem::path& path);

        ~GltfModel();
        GltfModel(GltfModel&&) noexcept;
        GltfModel& operator=(GltfModel&&) noexcept;

        GltfModel(const GltfModel&) = delete;
        GltfModel& operator=(const GltfModel&) = delete;

        [[nodiscard]]
        const std::filesystem::path& GetPath() const noexcept;

        // What the file holds (computed on first call). Problems with its skins or animations are
        // reported in the result rather than thrown, as the rest of the file may still import;
        // throws ImportError if the scene itself is malformed (e.g. its node hierarchy is not a
        // tree), which no import could get past.
        [[nodiscard]]
        const GltfModelInfo& GetInfo();

        // The whole scene as one static mesh (see above). Throws ImportError if the file is
        // malformed, references an external buffer or image that is missing, truncated or
        // undecodable, or contains no triangle geometry.
        [[nodiscard]]
        MeshData ImportMesh();

        // The file's skeleton (see above). Throws ImportError if its skins are malformed.
        [[nodiscard]]
        SkeletonData ImportSkeleton();

        // The scene's meshes bound to `skeleton`: the file's own (ImportSkeleton), or another one
        // with bones of the same names (e.g. one imported from another file exported from the same
        // rig), with rigid parts attached to bones as `attachments` says. Problems that do not stop
        // the import (skins disagreeing on a bone's bind pose, a part's chosen bone that does not
        // exist) are added to `warnings`. Throws ImportError as ImportMesh does, or if the mesh is
        // skinned to a bone that `skeleton` does not have.
        //
        // With `separateParts`, the parts attachments.separateParts names are left out of the
        // skeletal mesh and added there instead, one per part name (whose materials and textures
        // the skeletal mesh's lists hold; see GltfSeparatePart). Throws ImportError if nothing
        // would be left in the skeletal mesh.
        [[nodiscard]]
        SkeletalMeshData ImportSkeletalMesh(const SkeletonData& skeleton, std::vector<std::string>& warnings,
                                            const GltfAttachmentOptions& attachments = {},
                                            std::vector<GltfSeparatePart>* separateParts = nullptr);

        // Animation `index` (into GetInfo().animations) as a clip for `skeleton`, the file's own or
        // another with bones of the same names. Channels that animate nodes the skeleton has no
        // bone for, or morph target weights, are left out and reported in `warnings`. Throws
        // ImportError if the animation is malformed or moves none of the skeleton's bones.
        [[nodiscard]]
        AnimationClipData ImportAnimation(std::size_t index, const SkeletonData& skeleton, std::vector<std::string>& warnings);

    private:
        struct Impl;

        explicit GltfModel(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> m_Impl;
    };

    // Convenience for the common case: the geometry of a glTF file as a single static mesh, ready to
    // be uploaded as one Frost::Mesh (GltfModel::Open(path).ImportMesh()).
    class GltfImporter final {
    public:
        GltfImporter() = delete;

        // Throws ImportError if the file cannot be read, is not valid glTF 2.0 (.glb or .gltf),
        // uses an unsupported feature (see GltfModel), references an external buffer or image that
        // is missing, truncated or undecodable, or contains no triangle geometry.
        [[nodiscard]]
        static MeshData Import(const std::filesystem::path& path);
    };

}  // namespace Frost
