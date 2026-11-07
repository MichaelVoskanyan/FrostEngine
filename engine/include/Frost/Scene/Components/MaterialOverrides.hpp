#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Renderer/Material.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

    // The per-object material overrides of a drawn model, one optional override per sub-mesh, and
    // the materials built from them. Shared by the components that draw models (MeshComponent,
    // SkeletalMeshComponent).
    class MaterialOverrides final {
    public:
        // Number of sub-meshes that can carry an override.
        [[nodiscard]]
        std::size_t GetSlotCount() const noexcept;

        // One slot per sub-mesh of a new model; overrides for indices it also has are kept
        // (swapping a model for a re-exported version of itself keeps its materials).
        void Resize(std::size_t subMeshCount);

        // Throws std::out_of_range for an index past GetSlotCount().
        [[nodiscard]]
        const std::optional<MaterialOverride>& Get(std::size_t subMeshIndex) const;

        // Sets or clears (nullopt) sub-mesh `subMeshIndex`'s override. Indices past the current
        // count grow the list (used when loading an unresolved model's overrides).
        void Set(std::size_t subMeshIndex, std::optional<MaterialOverride> materialOverride);

        // Forces Resolve to rebuild (the model changed, or a texture an override uses was
        // reimported and must be loaded again).
        void Invalidate() noexcept;

        // The materials to draw `model` with, one slot per sub-mesh (null = the model's own), built
        // from the overrides (inheriting slots take `model`'s own material's textures). Rebuilt only
        // after something changed; a texture that cannot be loaded is reported (naming
        // `modelName`) and left at the slot's default. All null without a model.
        [[nodiscard]]
        const std::vector<std::shared_ptr<Material>>& Resolve(AssetManager& assets, const MeshAsset* model,
                                                              const std::string& modelName);

    private:
        std::vector<std::optional<MaterialOverride>> m_Overrides;
        std::vector<std::shared_ptr<Material>> m_Resolved;
        bool m_Dirty = true;
    };

}  // namespace Frost
