#pragma once

#include <Frost/Assets/Image.hpp>
#include <Frost/Renderer/Material.hpp>
#include <Frost/Renderer/Texture.hpp>
#include <Frost/Renderer/Vertex.hpp>

#include <vulkan/vulkan.h>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Frost {

    class VulkanAllocator;
    class VulkanDevice;
    class VulkanCommandPool;
    class VulkanBuffer;

    // One drawable range of a Mesh: a contiguous run of its index buffer, issued as a single
    // indexed draw. A mesh imported from a model file typically has one of these per source
    // primitive (glTF "primitive", i.e. a material slot), all sharing the mesh's vertex and index
    // buffers so the whole model uploads and binds as one object.
    struct SubMesh {
        std::uint32_t firstIndex = 0;  // Offset into the mesh's index buffer, in indices.
        std::uint32_t indexCount = 0;
        // Added to every index of the range before fetching the vertex (vkCmdDrawIndexed's
        // vertexOffset), so a primitive's indices can stay relative to its own first vertex.
        std::int32_t vertexOffset = 0;
        // Name from the source asset (glTF: the instancing node's name, else the mesh's), so a
        // sub-mesh can be addressed by Mesh::FindSubMesh. Not necessarily unique, may be empty.
        std::string name;
        // Index into MeshData::materials of the material this range was authored with, or -1 for
        // none (drawn with the default material). Only meaningful in MeshData; Mesh keeps its
        // materials in its own slots.
        std::int32_t materialIndex = -1;
    };

    // A texture as imported alongside geometry: decoded pixels plus how to sample them.
    struct TextureData {
        ImageData image;
        TextureSettings settings;
        std::string name;  // From the source asset; may be empty.
    };

    // A material as imported alongside geometry: its parameters plus, per texture slot, an index
    // into MeshData::textures or -1 for unset. Uploaded as a Material (see MaterialSystem).
    struct MaterialData {
        std::string name;  // From the source asset; may be empty.
        MaterialParameters parameters;
        std::int32_t baseColorTexture = -1;
        std::int32_t metallicRoughnessTexture = -1;
        std::int32_t normalTexture = -1;
        std::int32_t occlusionTexture = -1;
        std::int32_t emissiveTexture = -1;
    };

    // CPU-side geometry ready to be uploaded as a Mesh. Produced by importers (see GltfImporter)
    // or built by hand.
    struct MeshData {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        // Draw ranges over `indices`. Leaving this empty means "the whole index buffer as one
        // sub-mesh with vertexOffset 0".
        std::vector<SubMesh> subMeshes;
        // Materials referenced by SubMesh::materialIndex, and the textures they reference in
        // turn. Importers only include what some sub-mesh actually uses.
        std::vector<MaterialData> materials;
        std::vector<TextureData> textures;
    };

    // A drawable: a device-local vertex buffer + index buffer pair, uploaded once at construction,
    // plus the list of sub-mesh ranges drawn from them, per sub-mesh an optional material, and the
    // mesh's transform (its model matrix, identity by default). This is the base unit Renderer
    // submits draw calls for; a model/asset system builds Meshes from loaded geometry rather than
    // replacing this type.
    //
    // Materials are not created here (MeshData::materials/textures are CPU data; see
    // MaterialSystem for the GPU objects): whoever uploads the mesh creates the Materials it needs
    // and attaches them with SetMaterial, which is also how a material built by hand from
    // separately imported images is applied to a model that has none of its own.
    class Mesh final {
    public:
        // Throws std::invalid_argument if `data` has no vertices, no indices, or a sub-mesh whose
        // range lies outside the index buffer. (Index *values* are not validated here; importers
        // are responsible for producing in-range indices.)
        Mesh(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
             const MeshData& data);

        // Convenience for a single-range mesh (the whole index buffer, vertexOffset 0).
        Mesh(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
             const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices);
        ~Mesh();

        Mesh(const Mesh&) = delete;
        Mesh& operator=(const Mesh&) = delete;

        Mesh(Mesh&&) = delete;
        Mesh& operator=(Mesh&&) = delete;

        // Pushes this mesh's transform (as MeshPushConstants), binds its vertex/index buffers once,
        // and issues one indexed draw per sub-mesh, binding each sub-mesh's material first
        // (`fallbackMaterial` for sub-meshes that have none). `pipelineLayout` must be the engine's
        // graphics layout (MeshPushConstants, material set at index 1; see VulkanPipeline).
        void Draw(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout,
                  const Material& fallbackMaterial) const;

        // As above, but with an explicit model matrix instead of the mesh's own, and per-sub-mesh
        // material overrides: a non-null materialOverrides[i] replaces sub-mesh i's material (the
        // span may be shorter than the sub-mesh count, or empty). This is how one uploaded mesh is
        // drawn as many independently placed and textured objects (see Renderer::Submit).
        void Draw(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout, const glm::mat4& transform,
                  std::span<const std::shared_ptr<Material>> materialOverrides,
                  const Material& fallbackMaterial) const;

        // The model matrix (object -> world): the engine-side position/rotation/scale of this mesh.
        // Read at draw time, so it can change every frame.
        void SetTransform(const glm::mat4& transform) noexcept;

        [[nodiscard]]
        const glm::mat4& GetTransform() const noexcept;

        // Attaches `material` (or nothing, with nullptr) to the given sub-mesh. The mesh shares
        // ownership, so the material (and its textures) stays alive for as long as any sub-mesh
        // uses it. Throws std::out_of_range for a bad index. Safe to call between frames only:
        // materials bound by an in-flight command buffer must not be released until that frame
        // completes.
        void SetMaterial(std::size_t subMeshIndex, std::shared_ptr<Material> material);

        [[nodiscard]]
        const std::shared_ptr<Material>& GetMaterial(std::size_t subMeshIndex) const;

        // Index of the first sub-mesh whose name matches, if any.
        [[nodiscard]]
        std::optional<std::size_t> FindSubMesh(std::string_view name) const noexcept;

        [[nodiscard]]
        const std::vector<SubMesh>& GetSubMeshes() const noexcept;

        [[nodiscard]]
        std::uint32_t GetVertexCount() const noexcept;

        [[nodiscard]]
        std::uint32_t GetIndexCount() const noexcept;

    private:
        void Upload(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
                    const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices,
                    const std::vector<SubMesh>& subMeshes);

        std::unique_ptr<VulkanBuffer> m_VertexBuffer;
        std::unique_ptr<VulkanBuffer> m_IndexBuffer;
        std::vector<SubMesh> m_SubMeshes;
        std::vector<std::shared_ptr<Material>> m_Materials;  // One slot per sub-mesh; null = none.
        glm::mat4 m_Transform{1.0f};
        std::uint32_t m_VertexCount = 0;
        std::uint32_t m_IndexCount = 0;
    };

}  // namespace Frost
