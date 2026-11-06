#include <Frost/Scene/Components/MeshComponent.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Renderer/MaterialSystem.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <iostream>
#include <stdexcept>
#include <utility>

namespace Frost {

    bool IsColorSlot(const MaterialSlot slot) noexcept {
        return slot == MaterialSlot::BaseColor || slot == MaterialSlot::Emissive;
    }

    const char* GetMaterialSlotName(const MaterialSlot slot) noexcept {
        switch (slot) {
            case MaterialSlot::BaseColor: return "baseColor";
            case MaterialSlot::MetallicRoughness: return "metallicRoughness";
            case MaterialSlot::Normal: return "normal";
            case MaterialSlot::Occlusion: return "occlusion";
            case MaterialSlot::Emissive: return "emissive";
        }
        return "unknown";
    }

    MeshComponent::MeshComponent(std::shared_ptr<const MeshAsset> asset) { SetMesh(std::move(asset)); }

    void MeshComponent::SetMesh(std::shared_ptr<const MeshAsset> asset) {
        m_Mesh = std::move(asset);
        m_MeshReference = m_Mesh != nullptr ? AssetReference{.id = m_Mesh->id, .path = m_Mesh->path} : AssetReference{};

        // One override slot per sub-mesh of the new model; overrides for indices it also has are
        // kept (swapping a model for a re-exported version of itself keeps its materials).
        m_MaterialOverrides.resize(m_Mesh != nullptr ? m_Mesh->mesh->GetSubMeshes().size() : 0);
        m_MaterialsDirty = true;
    }

    void MeshComponent::SetUnresolvedMesh(AssetReference reference) {
        m_Mesh.reset();
        m_MeshReference = std::move(reference);
        m_MaterialsDirty = true;
    }

    const std::shared_ptr<const MeshAsset>& MeshComponent::GetMesh() const noexcept { return m_Mesh; }

    const AssetReference& MeshComponent::GetMeshReference() const noexcept { return m_MeshReference; }

    bool MeshComponent::IsMissing() const noexcept { return m_Mesh == nullptr && m_MeshReference.IsSet(); }

    std::optional<Bounds> MeshComponent::GetLocalBounds() const {
        if (m_Mesh == nullptr) { return std::nullopt; }
        return m_Mesh->bounds;
    }

    std::size_t MeshComponent::GetMaterialSlotCount() const noexcept { return m_MaterialOverrides.size(); }

    const std::optional<MaterialOverride>& MeshComponent::GetMaterialOverride(const std::size_t subMeshIndex) const {
        if (subMeshIndex >= m_MaterialOverrides.size()) {
            throw std::out_of_range("MeshComponent::GetMaterialOverride: sub-mesh index out of range.");
        }
        return m_MaterialOverrides[subMeshIndex];
    }

    void MeshComponent::SetMaterialOverride(const std::size_t subMeshIndex,
                                            std::optional<MaterialOverride> materialOverride) {
        if (subMeshIndex >= m_MaterialOverrides.size()) { m_MaterialOverrides.resize(subMeshIndex + 1); }

        m_MaterialOverrides[subMeshIndex] = std::move(materialOverride);
        m_MaterialsDirty = true;
    }

    void MeshComponent::InvalidateMaterials() noexcept { m_MaterialsDirty = true; }

    const std::vector<std::shared_ptr<Material>>& MeshComponent::GetResolvedMaterials(AssetManager& assets) {
        if (!m_MaterialsDirty) { return m_ResolvedMaterials; }
        m_MaterialsDirty = false;

        // The previous materials may still be in use by frames in flight; the renderer holds its
        // own references to them until those frames complete, so replacing them here is safe.
        m_ResolvedMaterials.assign(m_MaterialOverrides.size(), nullptr);
        if (m_Mesh == nullptr) { return m_ResolvedMaterials; }

        const std::size_t subMeshCount = m_Mesh->mesh->GetSubMeshes().size();

        for (std::size_t i = 0; i < m_MaterialOverrides.size() && i < subMeshCount; ++i) {
            const std::optional<MaterialOverride>& materialOverride = m_MaterialOverrides[i];
            if (!materialOverride.has_value()) { continue; }

            // The sub-mesh's own material, for slots that inherit (null: the model has none, so
            // inherited slots are simply unset).
            const std::shared_ptr<Material>& imported = m_Mesh->mesh->GetMaterial(i);

            std::array<std::shared_ptr<Texture>, kMaterialSlotCount> slots{};
            for (std::size_t slot = 0; slot < kMaterialSlotCount; ++slot) {
                const TextureOverride& texture = materialOverride->textures[slot];

                switch (texture.source) {
                    case TextureOverride::Source::Inherit:
                        if (imported != nullptr) {
                            const MaterialTextures& importedTextures = imported->GetTextures();
                            const std::array<std::shared_ptr<Texture>, kMaterialSlotCount> importedSlots = {
                                    importedTextures.baseColor, importedTextures.metallicRoughness,
                                    importedTextures.normal, importedTextures.occlusion, importedTextures.emissive};
                            slots[slot] = importedSlots[slot];
                        }
                        break;

                    case TextureOverride::Source::None: break;

                    case TextureOverride::Source::Asset:
                        try {
                            slots[slot] = assets.LoadTexture(texture.texture, IsColorSlot(static_cast<MaterialSlot>(slot)));
                        }
                        catch (const ImportError& e) {
                            std::cerr << "[Frost::MeshComponent] Model '" << m_Mesh->name << "': could not load the "
                                      << GetMaterialSlotName(static_cast<MaterialSlot>(slot))
                                      << " texture; leaving the slot at its default. " << e.what() << '\n';
                        }
                        break;
                }
            }

            m_ResolvedMaterials[i] = assets.GetMaterialSystem().CreateMaterial(
                    materialOverride->parameters, MaterialTextures{.baseColor = slots[0],
                                                                   .metallicRoughness = slots[1],
                                                                   .normal = slots[2],
                                                                   .occlusion = slots[3],
                                                                   .emissive = slots[4]});
        }

        return m_ResolvedMaterials;
    }

}  // namespace Frost
