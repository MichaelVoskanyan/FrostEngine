#include <Frost/Scene/Components/MaterialOverrides.hpp>

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

    std::size_t MaterialOverrides::GetSlotCount() const noexcept { return m_Overrides.size(); }

    void MaterialOverrides::Resize(const std::size_t subMeshCount) {
        m_Overrides.resize(subMeshCount);
        m_Dirty = true;
    }

    const std::optional<MaterialOverride>& MaterialOverrides::Get(const std::size_t subMeshIndex) const {
        if (subMeshIndex >= m_Overrides.size()) {
            throw std::out_of_range("MaterialOverrides::Get: sub-mesh index out of range.");
        }
        return m_Overrides[subMeshIndex];
    }

    void MaterialOverrides::Set(const std::size_t subMeshIndex, std::optional<MaterialOverride> materialOverride) {
        if (subMeshIndex >= m_Overrides.size()) { m_Overrides.resize(subMeshIndex + 1); }

        m_Overrides[subMeshIndex] = std::move(materialOverride);
        m_Dirty = true;
    }

    void MaterialOverrides::Invalidate() noexcept { m_Dirty = true; }

    const std::vector<std::shared_ptr<Material>>& MaterialOverrides::Resolve(AssetManager& assets, const MeshAsset* const model,
                                                                             const std::string& modelName) {
        if (!m_Dirty) { return m_Resolved; }
        m_Dirty = false;

        // The previous materials may still be in use by frames in flight; the renderer holds its
        // own references to them until those frames complete, so replacing them here is safe.
        m_Resolved.assign(m_Overrides.size(), nullptr);
        if (model == nullptr) { return m_Resolved; }

        const std::size_t subMeshCount = model->mesh->GetSubMeshes().size();

        for (std::size_t i = 0; i < m_Overrides.size() && i < subMeshCount; ++i) {
            const std::optional<MaterialOverride>& materialOverride = m_Overrides[i];
            if (!materialOverride.has_value()) { continue; }

            // The sub-mesh's own material, for slots that inherit (null: the model has none, so
            // inherited slots are simply unset).
            const std::shared_ptr<Material>& imported = model->mesh->GetMaterial(i);

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
                            std::cerr << "[Frost::MaterialOverrides] Model '" << modelName << "': could not load the "
                                      << GetMaterialSlotName(static_cast<MaterialSlot>(slot))
                                      << " texture; leaving the slot at its default. " << e.what() << '\n';
                        }
                        break;
                }
            }

            m_Resolved[i] = assets.GetMaterialSystem().CreateMaterial(materialOverride->parameters,
                                                                      MaterialTextures{.baseColor = slots[0],
                                                                                       .metallicRoughness = slots[1],
                                                                                       .normal = slots[2],
                                                                                       .occlusion = slots[3],
                                                                                       .emissive = slots[4]});
        }

        return m_Resolved;
    }

}  // namespace Frost
