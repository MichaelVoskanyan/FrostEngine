#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Renderer/Material.hpp>
#include <Frost/Scene/Bounds.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Frost {

    class AssetManager;
    struct MeshAsset;

    // Texture slots of a material, in MaterialTextures order.
    enum class MaterialSlot : std::uint8_t { BaseColor, MetallicRoughness, Normal, Occlusion, Emissive };

    inline constexpr std::size_t kMaterialSlotCount = MaterialTextures::kSlotCount;

    // True for the slots that hold color (sampled as sRGB), false for data slots.
    [[nodiscard]]
    bool IsColorSlot(MaterialSlot slot) noexcept;

    // Display/serialization name of a slot ("baseColor", "metallicRoughness", ...).
    [[nodiscard]]
    const char* GetMaterialSlotName(MaterialSlot slot) noexcept;

    // How one texture slot of a MaterialOverride is filled.
    struct TextureOverride {
        enum class Source : std::uint8_t {
            Inherit,  // Whatever the model's own material for this sub-mesh has in the slot.
            None,     // Nothing: the slot's neutral default (see MaterialSystem).
            Asset,    // The texture asset `texture` refers to.
        };

        Source source = Source::Inherit;
        AssetReference texture;  // Only meaningful for Asset.
    };

    // Replaces the material a model's sub-mesh was authored with, for one object only.
    struct MaterialOverride {
        MaterialParameters parameters;
        std::array<TextureOverride, kMaterialSlotCount> textures{};
    };

    // A renderable model (a mesh asset), drawn with the object's transform; each of its sub-meshes
    // may have its material overridden for this object.
    //
    // The model is referenced by asset (id + last known path), and the loaded asset is kept
    // alongside; the reference is kept even if the asset could not be loaded (a missing file), so
    // saving the level does not lose it. A component with no model at all is allowed (it is
    // waiting for one to be assigned) and draws nothing. Material overrides are stored per
    // sub-mesh index.
    class MeshComponent final {
    public:
        static constexpr const char* kDisplayName = "Mesh";

        MeshComponent() = default;

        // Showing a loaded model (see SetMesh).
        explicit MeshComponent(std::shared_ptr<const MeshAsset> asset);

        // Assigns a loaded model (or none, with nullptr). Material overrides are kept for the
        // sub-mesh indices the new model also has.
        void SetMesh(std::shared_ptr<const MeshAsset> asset);

        // Records a model reference that could not be loaded: nothing is drawn, but the reference
        // (and any overrides) survive a save.
        void SetUnresolvedMesh(AssetReference reference);

        // The loaded model; null if there is none or it could not be loaded.
        [[nodiscard]]
        const std::shared_ptr<const MeshAsset>& GetMesh() const noexcept;

        // The referenced model, whether or not it is loaded; unset if no model is assigned.
        [[nodiscard]]
        const AssetReference& GetMeshReference() const noexcept;

        // True when a model is referenced but could not be loaded.
        [[nodiscard]]
        bool IsMissing() const noexcept;

        // The model's box in the object's own space, or nullopt if none is loaded.
        [[nodiscard]]
        std::optional<Bounds> GetLocalBounds() const;

        // Number of sub-meshes that can carry an override: the model's sub-mesh count, or (for an
        // unresolved model) however many overrides were recorded.
        [[nodiscard]]
        std::size_t GetMaterialSlotCount() const noexcept;

        [[nodiscard]]
        const std::optional<MaterialOverride>& GetMaterialOverride(std::size_t subMeshIndex) const;

        // Sets or clears (nullopt) sub-mesh `subMeshIndex`'s override. Indices past the current
        // count grow the list (used when loading an unresolved model's overrides).
        void SetMaterialOverride(std::size_t subMeshIndex, std::optional<MaterialOverride> materialOverride);

        // Forces GetResolvedMaterials to rebuild (e.g. after a texture an override uses was
        // reimported and must be loaded again).
        void InvalidateMaterials() noexcept;

        // The materials to draw the model with, one slot per sub-mesh (null = the model's own),
        // built from the overrides. Rebuilt only after something changed; a texture that cannot
        // be loaded is reported once and left at the slot's default.
        [[nodiscard]]
        const std::vector<std::shared_ptr<Material>>& GetResolvedMaterials(AssetManager& assets);

    private:
        AssetReference m_MeshReference;
        std::shared_ptr<const MeshAsset> m_Mesh;
        std::vector<std::optional<MaterialOverride>> m_MaterialOverrides;

        std::vector<std::shared_ptr<Material>> m_ResolvedMaterials;
        bool m_MaterialsDirty = true;
    };

}  // namespace Frost
