#include <Frost/Renderer/Mesh.hpp>

#include <Frost/Renderer/ShaderData.hpp>
#include <Frost/Renderer/Vulkan/VulkanBuffer.hpp>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

    // Descriptor set index of the material in the graphics pipeline layout (set 0 is per-frame).
    constexpr std::uint32_t kMaterialSetIndex = 1;

}  // namespace

namespace Frost {

    Mesh::Mesh(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
               const MeshData& data) {
        Upload(allocator, device, commandPool, data.vertices, data.indices, data.subMeshes);
    }

    Mesh::Mesh(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
               const MeshData& data, const std::span<const SkinnedVertex> skin) {
        if (skin.size() != data.vertices.size()) {
            throw std::invalid_argument("Mesh: a skinned mesh needs one skin entry per vertex.");
        }
        Upload(allocator, device, commandPool, data.vertices, data.indices, data.subMeshes);

        // The palette a draw must supply: every joint a weighted influence refers to.
        for (const SkinnedVertex& vertex : skin) {
            for (std::size_t i = 0; i < vertex.joints.size(); ++i) {
                if (vertex.weights[i] != 0.0f) { m_JointCount = std::max<std::uint32_t>(m_JointCount, vertex.joints[i] + 1u); }
            }
        }
        m_SkinBuffer = VulkanBuffer::CreateDeviceLocal(allocator, device, commandPool, skin.data(),
                                                        sizeof(SkinnedVertex) * skin.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }

    Mesh::Mesh(const VulkanAllocator& allocator, const VulkanDevice& device, const VulkanCommandPool& commandPool,
               const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
        Upload(allocator, device, commandPool, vertices, indices, {});
    }

    Mesh::~Mesh() = default;

    void Mesh::Upload(const VulkanAllocator& allocator, const VulkanDevice& device,
                      const VulkanCommandPool& commandPool, const std::vector<Vertex>& vertices,
                      const std::vector<std::uint32_t>& indices, const std::vector<SubMesh>& subMeshes) {
        // Vulkan rejects zero-sized buffers, and the counts below are carried as 32-bit values.
        if (vertices.empty() || indices.empty()) {
            throw std::invalid_argument("Mesh requires at least one vertex and one index.");
        }
        if (vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            indices.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Mesh vertex/index counts exceed the 32-bit limit.");
        }

        m_VertexCount = static_cast<std::uint32_t>(vertices.size());
        m_IndexCount = static_cast<std::uint32_t>(indices.size());

        if (subMeshes.empty()) {
            m_SubMeshes = {SubMesh{.firstIndex = 0,
                                   .indexCount = m_IndexCount,
                                   .vertexOffset = 0,
                                   .name = {},
                                   .materialIndex = -1}};
        }
        else {
            for (std::size_t i = 0; i < subMeshes.size(); ++i) {
                const SubMesh& subMesh = subMeshes[i];

                // Checked in 64-bit so firstIndex + indexCount cannot wrap.
                const std::uint64_t end =
                        static_cast<std::uint64_t>(subMesh.firstIndex) + static_cast<std::uint64_t>(subMesh.indexCount);

                if (subMesh.indexCount == 0 || end > m_IndexCount || subMesh.vertexOffset < 0 ||
                    static_cast<std::uint32_t>(subMesh.vertexOffset) >= m_VertexCount) {
                    std::ostringstream message;
                    message << "Mesh sub-mesh " << i << " (firstIndex " << subMesh.firstIndex << ", indexCount "
                            << subMesh.indexCount << ", vertexOffset " << subMesh.vertexOffset
                            << ") lies outside the mesh's " << m_IndexCount << " indices / " << m_VertexCount
                            << " vertices.";
                    throw std::invalid_argument(message.str());
                }
            }

            m_SubMeshes = subMeshes;
        }

        m_Materials.assign(m_SubMeshes.size(), nullptr);

        const VkDeviceSize vertexBufferSize = sizeof(Vertex) * vertices.size();
        const VkDeviceSize indexBufferSize = sizeof(std::uint32_t) * indices.size();

        m_VertexBuffer = VulkanBuffer::CreateDeviceLocal(allocator, device, commandPool, vertices.data(),
                                                          vertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        m_IndexBuffer = VulkanBuffer::CreateDeviceLocal(allocator, device, commandPool, indices.data(),
                                                        indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    void Mesh::Draw(const VkCommandBuffer commandBuffer, const VkPipelineLayout pipelineLayout,
                    const Material& fallbackMaterial) const {
        Draw(commandBuffer, pipelineLayout, m_Transform, {}, fallbackMaterial);
    }

    void Mesh::Draw(const VkCommandBuffer commandBuffer, const VkPipelineLayout pipelineLayout,
                    const glm::mat4& transform, const std::span<const std::shared_ptr<Material>> materialOverrides,
                    const Material& fallbackMaterial, const std::uint32_t firstJoint) const {
        // Per-draw constants. The normal matrix is the inverse-transpose of the model's linear
        // part, so normals stay perpendicular under non-uniform scale; a singular transform has no
        // meaningful normals anyway, so fall back to the linear part rather than produce NaNs.
        const glm::mat3 linear(transform);
        MeshPushConstants pushConstants{
                .model = transform,
                .normalMatrix = glm::determinant(linear) != 0.0f ? glm::mat4(glm::inverseTranspose(linear))
                                                                 : glm::mat4(linear),
        };
        if (IsSkinned()) { SetFirstJoint(pushConstants, firstJoint); }
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants),
                           &pushConstants);

        // The skin stream, if any, at binding 1 (see SkinnedVertex).
        const std::array<VkBuffer, 2> vertexBuffers = {m_VertexBuffer->GetHandle(),
                                                       m_SkinBuffer != nullptr ? m_SkinBuffer->GetHandle() : VK_NULL_HANDLE};
        const std::array<VkDeviceSize, 2> offsets = {0, 0};

        vkCmdBindVertexBuffers(commandBuffer, 0, IsSkinned() ? 2 : 1, vertexBuffers.data(), offsets.data());
        vkCmdBindIndexBuffer(commandBuffer, m_IndexBuffer->GetHandle(), 0, VK_INDEX_TYPE_UINT32);

        VkDescriptorSet boundSet = VK_NULL_HANDLE;

        for (std::size_t i = 0; i < m_SubMeshes.size(); ++i) {
            const SubMesh& subMesh = m_SubMeshes[i];

            // Override, else the mesh's own material, else the fallback.
            const Material* material = i < materialOverrides.size() ? materialOverrides[i].get() : nullptr;
            if (material == nullptr) { material = m_Materials[i].get(); }
            if (material == nullptr) { material = &fallbackMaterial; }

            // Consecutive sub-meshes commonly share a material; skip the redundant rebind.
            const VkDescriptorSet set = material->GetDescriptorSet();
            if (set != boundSet) {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                        kMaterialSetIndex, 1, &set, 0, nullptr);
                boundSet = set;
            }

            vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, 1, subMesh.firstIndex, subMesh.vertexOffset, 0);
        }
    }

    bool Mesh::IsSkinned() const noexcept { return m_SkinBuffer != nullptr; }

    std::uint32_t Mesh::GetJointCount() const noexcept { return m_JointCount; }

    void Mesh::SetTransform(const glm::mat4& transform) noexcept { m_Transform = transform; }

    const glm::mat4& Mesh::GetTransform() const noexcept { return m_Transform; }

    void Mesh::SetMaterial(const std::size_t subMeshIndex, std::shared_ptr<Material> material) {
        if (subMeshIndex >= m_Materials.size()) {
            throw std::out_of_range("Mesh::SetMaterial: sub-mesh index out of range.");
        }
        m_Materials[subMeshIndex] = std::move(material);
    }

    const std::shared_ptr<Material>& Mesh::GetMaterial(const std::size_t subMeshIndex) const {
        if (subMeshIndex >= m_Materials.size()) {
            throw std::out_of_range("Mesh::GetMaterial: sub-mesh index out of range.");
        }
        return m_Materials[subMeshIndex];
    }

    std::optional<std::size_t> Mesh::FindSubMesh(const std::string_view name) const noexcept {
        for (std::size_t i = 0; i < m_SubMeshes.size(); ++i) {
            if (m_SubMeshes[i].name == name) { return i; }
        }
        return std::nullopt;
    }

    const std::vector<SubMesh>& Mesh::GetSubMeshes() const noexcept { return m_SubMeshes; }

    std::uint32_t Mesh::GetVertexCount() const noexcept { return m_VertexCount; }

    std::uint32_t Mesh::GetIndexCount() const noexcept { return m_IndexCount; }

}  // namespace Frost
