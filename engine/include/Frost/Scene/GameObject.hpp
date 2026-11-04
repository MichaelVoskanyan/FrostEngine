#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Renderer/Light.hpp>
#include <Frost/Renderer/Material.hpp>
#include <Frost/Scene/Bounds.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Frost {

    class AssetManager;
    struct MeshAsset;

    // Unique (within a level) and stable (saved with the level) object identifier. 0 is never a
    // valid id.
    using ObjectId = std::uint64_t;

    // Position, rotation and scale, composed as translate * rotate * scale.
    //
    // Rotation is stored as Euler angles in degrees, because that is what people edit and what
    // round-trips through a level file without drift: x = pitch (about X), y = yaw (about Y),
    // z = roll (about Z), applied roll first, then pitch, then yaw (R = Ry * Rx * Rz), so yaw is
    // always about the world's up axis.
    struct Transform {
        glm::vec3 position{0.0f};
        glm::vec3 rotation{0.0f};
        glm::vec3 scale{1.0f};

        [[nodiscard]]
        glm::mat4 ToMatrix() const noexcept;

        // The rotation as a quaternion (R = Ry * Rx * Rz, see above).
        [[nodiscard]]
        glm::quat GetRotation() const noexcept;

        // The object's forward direction in world space: its local -Z rotated (unit length). Lights
        // shine this way, as the camera looks this way.
        [[nodiscard]]
        glm::vec3 GetForward() const noexcept;
    };

    // A light attached to an object. It sits at the object's position and shines along the
    // object's forward direction (Transform::GetForward), so placing and aiming a light is just
    // moving and rotating its object.
    struct LightComponent {
        LightType type = LightType::Point;
        bool enabled = true;

        // Linear RGB tint. With useTemperature, multiplied by the black-body color of `temperature`.
        glm::vec3 color{1.0f};
        bool useTemperature = false;
        float temperature = 6500.0f;  // Kelvin.

        // Brightness. Directional: illuminance on a surface facing the light (3 lights a white
        // surface to about white). Point/spot: intensity at 1 unit, falling off with the inverse
        // square of the distance.
        float intensity = 20.0f;

        // Point: radius of influence. Spot: how far the cone reaches. Lighting fades smoothly to
        // zero at this distance.
        float range = 10.0f;

        // Spot: cone half-angles in degrees. Full brightness inside the inner cone, fading to dark
        // at the outer one. 0 <= inner <= outer < 90.
        float innerConeAngle = 20.0f;
        float outerConeAngle = 30.0f;

        // Sensible starting values for a new light of each type.
        [[nodiscard]]
        static LightComponent CreateDefault(LightType type) noexcept;

        // The final linear color * intensity.
        [[nodiscard]]
        glm::vec3 GetRadiance() const noexcept;

        // The light as the renderer consumes it, placed by `transform`.
        [[nodiscard]]
        Light ToRenderLight(const Transform& transform) const noexcept;
    };

    // Display/serialization name of a light type ("directional", "point", "spot").
    [[nodiscard]]
    const char* GetLightTypeName(LightType type) noexcept;

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

    // An object placed in a level: a name, a transform, optionally a model to draw (whose sub-meshes
    // may each have their material overridden) and optionally a light.
    //
    // The model is referenced by asset (id + last known path), and the loaded asset is kept
    // alongside; the reference is kept even if the asset could not be loaded (a missing file), so
    // saving the level does not lose it. Material overrides are stored per sub-mesh index.
    class GameObject final {
    public:
        GameObject(ObjectId id, std::string name);

        [[nodiscard]]
        ObjectId GetId() const noexcept;

        [[nodiscard]]
        const std::string& GetName() const noexcept;

        void SetName(std::string name);

        [[nodiscard]]
        Transform& GetTransform() noexcept;

        [[nodiscard]]
        const Transform& GetTransform() const noexcept;

        // The object's light, if it has one; edit it in place, or assign nullopt to remove it.
        [[nodiscard]]
        std::optional<LightComponent>& GetLight() noexcept;

        [[nodiscard]]
        const std::optional<LightComponent>& GetLight() const noexcept;

        // Assigns a loaded model (or none, with nullptr). Material overrides are kept for the
        // sub-mesh indices the new model also has.
        void SetMesh(std::shared_ptr<const MeshAsset> asset);

        // Records a model reference that could not be loaded: nothing is drawn, but the reference
        // (and any overrides) survive a save.
        void SetUnresolvedMesh(AssetReference reference);

        [[nodiscard]]
        const std::shared_ptr<const MeshAsset>& GetMesh() const noexcept;

        // The referenced model, whether or not it is loaded; unset if the object has no model.
        [[nodiscard]]
        const AssetReference& GetMeshReference() const noexcept;

        // World-space box around the object's model, or nullopt if it has none loaded.
        [[nodiscard]]
        std::optional<Bounds> GetWorldBounds() const;

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
        ObjectId m_Id;
        std::string m_Name;
        Transform m_Transform;
        std::optional<LightComponent> m_Light;

        AssetReference m_MeshReference;
        std::shared_ptr<const MeshAsset> m_Mesh;
        std::vector<std::optional<MaterialOverride>> m_MaterialOverrides;

        std::vector<std::shared_ptr<Material>> m_ResolvedMaterials;
        bool m_MaterialsDirty = true;
    };

}  // namespace Frost
