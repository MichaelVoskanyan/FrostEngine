#pragma once

#include <Frost/Animation/AnimationClip.hpp>
#include <Frost/Animation/Skeleton.hpp>
#include <Frost/Animation/SkinnedMesh.hpp>
#include <Frost/Animation/Socket.hpp>
#include <Frost/Assets/AssetId.hpp>
#include <Frost/Assets/AssetType.hpp>
#include <Frost/Assets/Image.hpp>
#include <Frost/Assets/ImportError.hpp>
#include <Frost/Renderer/Material.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Frost {

    // What every asset file starts with (see AssetFile::ReadHeader).
    struct AssetHeader {
        AssetType type{};
        std::uint32_t version = 0;
        // The asset's permanent identity. Invalid only in version-1 files, which predate ids (see
        // AssetImporter::UpgradeLegacyAsset).
        AssetId id;
        // The file it was imported from (resolved to an absolute path), for reimporting, and which
        // item inside that file (e.g. one of a model's textures); both empty if unknown / the whole
        // file.
        std::filesystem::path sourcePath;
        std::string sourceItem;
    };

    // A material as a mesh asset stores it: parameters, and per texture slot a reference to a
    // texture asset (unset = the slot's neutral default).
    struct MaterialAssetData {
        std::string name;
        MaterialParameters parameters;
        std::array<AssetReference, MaterialTextures::kSlotCount> textures{};
    };

    // The contents of a .fmesh.
    struct MeshFile {
        AssetHeader header;
        // Vertices, indices and sub-meshes; each sub-mesh's materialIndex indexes `materials`.
        // (For version-1 files only: the materials and textures the file embedded are in
        // mesh.materials/mesh.textures instead, and `materials` is empty.)
        MeshData mesh;
        std::vector<MaterialAssetData> materials;
    };

    // The contents of a .ftex.
    struct TextureFile {
        AssetHeader header;
        ImageData image;
        TextureSettings settings;
    };

    // The contents of a .fskel.
    struct SkeletonFile {
        AssetHeader header;
        SkeletonData skeleton;
    };

    // A part that comes with a skeletal mesh as an object of its own (a part of the model imported
    // as a separate mesh; see GltfSeparatePart): when the skeletal mesh is placed in a level, the
    // editor attaches an object showing `mesh` to it (see AttachmentComponent), with no offset.
    struct DefaultAttachment {
        std::string name;    // The object's name (the part's).
        std::string slot;    // Its role, for animations (see AttachmentComponent::slot).
        std::string bone;    // The bone it moves with; empty = the skeletal mesh's object itself.
        AssetReference mesh;  // The .fmesh it shows.
    };

    // The contents of a .fskmesh: a mesh, the skin binding it to a skeleton, and that skeleton.
    struct SkeletalMeshFile {
        AssetHeader header;
        // The skeleton asset the mesh was imported for (what it is posed with).
        AssetReference skeleton;
        // Geometry in the bind pose, skin and bind table (see SkeletalMeshData). The sub-meshes'
        // materialIndex indexes `materials`; the mesh's own materials/textures lists are empty.
        SkeletalMeshData mesh;
        std::vector<MaterialAssetData> materials;
        // The parts it comes with as separate objects (version 3).
        std::vector<DefaultAttachment> defaultAttachments;
        // Its sockets (version 4), authored in the editor; kept when it is reimported.
        std::vector<SocketData> sockets;
    };

    // The contents of a .fanim.
    struct AnimationFile {
        AssetHeader header;
        // The skeleton asset the animation was imported for. Its tracks find their bones by name, so
        // it plays on any skeleton with bones of those names.
        AssetReference skeleton;
        AnimationClipData clip;
    };

    // Frost's own asset files: what the editor's importers produce from source files (glTF, PNG,
    // ...) and what levels and the runtime load. They hold the engine's data structures directly,
    // so loading one is a straight read with no parsing of third-party formats, and every source
    // format the importers learn later (FBX, ...) ends up in the same files.
    //
    //   .fmesh   - geometry, sub-meshes and materials. Materials refer to their textures, which
    //              are separate .ftex assets, by reference (id + last known path).
    //   .ftex    - an RGBA8 image plus the sampler settings it was imported with.
    //   .fskel   - a skeleton: bones (name, parent, rest pose), parents first.
    //   .fskmesh - a skeletal mesh: a reference to its skeleton, bind-pose geometry and sub-meshes,
    //              per vertex up to four bone influences, the bind table (bone names and inverse
    //              bind matrices the influences index), the sub-meshes attached to bones, and
    //              materials as in a .fmesh.
    //   .fanim   - an animation: a reference to the skeleton it was made for, its duration, and per
    //              animated bone (by name) translation, rotation and scale keyframe channels.
    //
    // Layout: little-endian throughout. A common header (version 2 and later for meshes and
    // textures; every version of the others), then the type's payload:
    //   char[4] magic ("FMSH" / "FTEX" / "FSKL" / "FSKM" / "FANM"), u32 version, u64 idHigh,
    //   u64 idLow, string sourcePath, string sourceItem
    // where a string is a u32 byte length followed by UTF-8 bytes. The paths a file carries (its
    // source, a material's texture paths) are stored relative to the project directory when inside
    // it (see MakeProjectPath), so they stay valid when assets are moved around the project or the
    // project is moved; every function here takes that directory (conventionally the content
    // directory's parent, see AssetRegistry::GetProjectDirectory). The id sits at a fixed offset
    // (8) so it can be rewritten in place.
    //
    // Payloads (a reference is u64 idHigh, u64 idLow, string path; vec3/quat/mat4 are 3/4/16 f32,
    // quaternions in x, y, z, w order and matrices column-major):
    //   .fskel:   u32 boneCount, per bone: string name, i32 parent, vec3 translation, quat rotation,
    //             vec3 scale, (version 2:) u8 flags (1 = added in the editor).
    //   .fskmesh: reference skeleton, geometry (as .fmesh), u32 vertexCount then per vertex
    //             u16[4] bones and f32[4] weights, u32 bindCount then per entry string bone and mat4
    //             inverseBind, (version 2:) u32 attachmentCount then per sub-mesh i32 attachment
    //             (bind table entry, -1 = none), (version 3:) u32 defaultAttachmentCount then per
    //             attachment string name, string slot, string bone, reference mesh; (version 4:)
    //             u32 socketCount then per socket string name, string bone, vec3 position, vec3
    //             rotation (degrees), vec3 scale; materials (as .fmesh).
    //   .fanim:   reference skeleton, f32 duration, u32 trackCount, per track: string bone, then
    //             the translation, rotation and scale channels, each u8 interpolation, u32
    //             keyCount, f32[keyCount] times, and keyCount (x3 for cubic splines) vec3/quat values;
    //             (version 2:) u32 visibilityKeyCount, per key: f32 time, string slot, u8 visible.
    //
    // Mesh positions are in world units (centimeters, see Frost/Units.hpp) from mesh version 3;
    // versions 1 and 2 stored them in meters (glTF's unit), and are converted as they are read, so
    // every mesh reads back in centimeters whatever its version. Skeletons, skeletal meshes and
    // animations are in centimeters from their first version.
    //
    // Version 1 of meshes and textures (no id, no source item; meshes embedded their textures) is
    // still read (see HasId). Readers
    // reject newer versions, and validate every count and index against the file, so a truncated
    // or corrupt asset is reported (ImportError) rather than read out of bounds.
    class AssetFile final {
    public:
        AssetFile() = delete;

        static constexpr const char* kMeshExtension = ".fmesh";
        static constexpr const char* kTextureExtension = ".ftex";
        static constexpr const char* kSkeletonExtension = ".fskel";
        static constexpr const char* kSkeletalMeshExtension = ".fskmesh";
        static constexpr const char* kAnimationExtension = ".fanim";

        // 3: positions in centimeters (before: meters).
        static constexpr std::uint32_t kMeshVersion = 3;
        static constexpr std::uint32_t kTextureVersion = 2;
        // 2: bones added in the editor are flagged.
        static constexpr std::uint32_t kSkeletonVersion = 2;
        // 4: sockets. 3: default attachments. 2: sub-meshes attached to bones.
        static constexpr std::uint32_t kSkeletalMeshVersion = 4;
        // 2: visibility keys.
        static constexpr std::uint32_t kAnimationVersion = 2;

        // The file extension of an asset type (".fmesh", ".ftex", ...).
        [[nodiscard]]
        static const char* GetExtension(AssetType type) noexcept;

        // The current format version of an asset type.
        [[nodiscard]]
        static std::uint32_t GetCurrentVersion(AssetType type) noexcept;

        // Whether files of this type and version carry an id: all but version-1 meshes and
        // textures, which predate ids (see AssetImporter::UpgradeLegacyAsset).
        [[nodiscard]]
        static bool HasId(AssetType type, std::uint32_t version) noexcept;

        // Write the current version (the header's type/version are ignored; its id must be valid).
        // Atomic (a temporary file renamed over the target), creating the directory if needed.
        // Throw ImportError if the file cannot be written.
        static void WriteMesh(const std::filesystem::path& path, const MeshFile& mesh,
                              const std::filesystem::path& projectDirectory);
        static void WriteTexture(const std::filesystem::path& path, const TextureFile& texture,
                                 const std::filesystem::path& projectDirectory);
        // These also validate what they are given (see SkeletonData::Validate, ValidateSkin and
        // AnimationClipData::Validate) and throw ImportError rather than write a file that would
        // not read back.
        static void WriteSkeleton(const std::filesystem::path& path, const SkeletonFile& skeleton,
                                  const std::filesystem::path& projectDirectory);
        static void WriteSkeletalMesh(const std::filesystem::path& path, const SkeletalMeshFile& mesh,
                                      const std::filesystem::path& projectDirectory);
        static void WriteAnimation(const std::filesystem::path& path, const AnimationFile& animation,
                                   const std::filesystem::path& projectDirectory);

        // Throw ImportError if the file is missing, not of this type, from a newer version, or
        // malformed.
        [[nodiscard]]
        static MeshFile ReadMesh(const std::filesystem::path& path, const std::filesystem::path& projectDirectory);

        [[nodiscard]]
        static TextureFile ReadTexture(const std::filesystem::path& path, const std::filesystem::path& projectDirectory);

        [[nodiscard]]
        static SkeletonFile ReadSkeleton(const std::filesystem::path& path, const std::filesystem::path& projectDirectory);

        [[nodiscard]]
        static SkeletalMeshFile ReadSkeletalMesh(const std::filesystem::path& path,
                                                 const std::filesystem::path& projectDirectory);

        [[nodiscard]]
        static AnimationFile ReadAnimation(const std::filesystem::path& path, const std::filesystem::path& projectDirectory);

        // Reads only the header. Nullopt if the file cannot be read or is not a Frost asset.
        [[nodiscard]]
        static std::optional<AssetHeader> ReadHeader(const std::filesystem::path& path,
                                                     const std::filesystem::path& projectDirectory);

        // Gives an existing asset file that has an id (see HasId) a different one, in place; e.g. for
        // a copy of an asset, which must not share the original's identity. Throws ImportError.
        static void RewriteId(const std::filesystem::path& path, const AssetId& id);
    };

}  // namespace Frost
