#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Renderer/Material.hpp>
#include <Frost/Scene/Bounds.hpp>
#include <Frost/Scene/Components/MaterialOverrides.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Frost {

    class AssetManager;
    struct MeshAsset;

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

        // --- Visibility ---------------------------------------------------------------------------

        // Whether the model is drawn (and can be clicked in the viewport); saved with the level. An
        // animation of the object it is attached to can override it while it plays (see
        // AnimationClipData::visibilityKeys).
        [[nodiscard]]
        bool IsVisible() const noexcept;

        void SetVisible(bool visible) noexcept;

        // The playing animation's say (run-time only, not saved): shown, hidden, or nullopt (no say:
        // IsVisible decides).
        [[nodiscard]]
        const std::optional<bool>& GetAnimationVisibility() const noexcept;

        void SetAnimationVisibility(std::optional<bool> visible) noexcept;

        // Whether it is drawn now: the animation's say, else IsVisible.
        [[nodiscard]]
        bool IsShown() const noexcept;

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
        MaterialOverrides m_MaterialOverrides;
        bool m_Visible = true;
        std::optional<bool> m_AnimationVisibility;
    };

}  // namespace Frost
