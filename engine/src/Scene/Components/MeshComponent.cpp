#include <Frost/Scene/Components/MeshComponent.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <string>
#include <utility>

namespace Frost {

    MeshComponent::MeshComponent(std::shared_ptr<const MeshAsset> asset) { SetMesh(std::move(asset)); }

    void MeshComponent::SetMesh(std::shared_ptr<const MeshAsset> asset) {
        m_Mesh = std::move(asset);
        m_MeshReference = m_Mesh != nullptr ? AssetReference{.id = m_Mesh->id, .path = m_Mesh->path} : AssetReference{};

        // One override slot per sub-mesh of the new model; overrides for indices it also has are
        // kept (swapping a model for a re-exported version of itself keeps its materials).
        m_MaterialOverrides.Resize(m_Mesh != nullptr ? m_Mesh->mesh->GetSubMeshes().size() : 0);
    }

    void MeshComponent::SetUnresolvedMesh(AssetReference reference) {
        m_Mesh.reset();
        m_MeshReference = std::move(reference);
        m_MaterialOverrides.Invalidate();
    }

    const std::shared_ptr<const MeshAsset>& MeshComponent::GetMesh() const noexcept { return m_Mesh; }

    const AssetReference& MeshComponent::GetMeshReference() const noexcept { return m_MeshReference; }

    bool MeshComponent::IsMissing() const noexcept { return m_Mesh == nullptr && m_MeshReference.IsSet(); }

    std::optional<Bounds> MeshComponent::GetLocalBounds() const {
        if (m_Mesh == nullptr) { return std::nullopt; }
        return m_Mesh->bounds;
    }

    bool MeshComponent::IsVisible() const noexcept { return m_Visible; }

    void MeshComponent::SetVisible(const bool visible) noexcept { m_Visible = visible; }

    const std::optional<bool>& MeshComponent::GetAnimationVisibility() const noexcept { return m_AnimationVisibility; }

    void MeshComponent::SetAnimationVisibility(std::optional<bool> visible) noexcept { m_AnimationVisibility = visible; }

    bool MeshComponent::IsShown() const noexcept { return m_AnimationVisibility.value_or(m_Visible); }

    std::size_t MeshComponent::GetMaterialSlotCount() const noexcept { return m_MaterialOverrides.GetSlotCount(); }

    const std::optional<MaterialOverride>& MeshComponent::GetMaterialOverride(const std::size_t subMeshIndex) const {
        return m_MaterialOverrides.Get(subMeshIndex);
    }

    void MeshComponent::SetMaterialOverride(const std::size_t subMeshIndex,
                                            std::optional<MaterialOverride> materialOverride) {
        m_MaterialOverrides.Set(subMeshIndex, std::move(materialOverride));
    }

    void MeshComponent::InvalidateMaterials() noexcept { m_MaterialOverrides.Invalidate(); }

    const std::vector<std::shared_ptr<Material>>& MeshComponent::GetResolvedMaterials(AssetManager& assets) {
        return m_MaterialOverrides.Resolve(assets, m_Mesh.get(), m_Mesh != nullptr ? m_Mesh->name : std::string());
    }

}  // namespace Frost
