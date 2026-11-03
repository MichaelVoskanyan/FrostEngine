#pragma once

#include <Frost/Assets/ImportError.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <filesystem>

namespace Frost {

    // Imports the geometry of a glTF 2.0 file as a single MeshData, ready to be uploaded as one
    // Frost::Mesh. Both containers are accepted: binary .glb (JSON + BIN chunk in one file) and
    // JSON .gltf, whose buffers and images are fetched from the URIs it lists (base64 data: URIs,
    // or files relative to the .gltf such as its .bin). A broken link (missing or truncated
    // external file) is reported as an ImportError naming the file, never a crash.
    //
    // The default scene (or the first one, or every root node if the file declares no scenes) is
    // walked depth-first. Every mesh primitive reached becomes one SubMesh: its vertices are
    // converted to the engine's Vertex layout and transformed by the instancing node's world
    // matrix (the product of the node chain's local matrix / translation-rotation-scale
    // transforms), so the output is the model as posed in the file, in a single object space.
    // Normals are transformed by the inverse-transpose and renormalized; a mirroring transform
    // (negative determinant) has its triangle winding flipped so front faces stay
    // counter-clockwise. A node with several mesh references, or a mesh instanced by several
    // nodes, yields one SubMesh per instance.
    //
    // Supported: TRIANGLES, TRIANGLE_STRIP and TRIANGLE_FAN primitives (strips/fans are expanded
    // to lists), indexed and non-indexed geometry, every accessor component type (including the
    // normalized integer formats of KHR_mesh_quantization), interleaved buffer views (byteStride),
    // POSITION, NORMAL, TEXCOORD_0 and COLOR_0 (vec3 or vec4) attributes. Missing normals are
    // generated flat, per the glTF specification (the primitive is de-indexed to do so). Missing
    // texture coordinates read as (0, 0); missing colors read as opaque white.
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
    // Tangents (for normal mapping) are taken from the TANGENT attribute when present and
    // otherwise derived from the UV layout, so every textured primitive gets a tangent frame.
    //
    // Not supported (throws ImportError): absolute or remote (http:, ...) URIs, sparse accessors,
    // and Draco / meshopt compressed geometry. Point and line primitives, morph targets, skins and
    // cameras are ignored.
    //
    // Sub-meshes are named after their node (or mesh, for unnamed nodes), so a specific part of a
    // model can be found with Mesh::FindSubMesh, e.g. to apply a texture imported separately.
    class GltfImporter final {
    public:
        GltfImporter() = delete;

        // Throws ImportError if the file cannot be read, is not valid glTF 2.0 (.glb or .gltf),
        // uses an unsupported feature (see above), references an external buffer or image that is
        // missing, truncated or undecodable, or contains no triangle geometry.
        [[nodiscard]]
        static MeshData Import(const std::filesystem::path& path);
    };

}  // namespace Frost
