#include <Frost/Scene/GameObject.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Renderer/MaterialSystem.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace Frost {

    glm::mat4 Transform::ToMatrix() const noexcept {
        return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(GetRotation()) *
               glm::scale(glm::mat4(1.0f), scale);
    }

    glm::quat Transform::GetRotation() const noexcept {
        const glm::quat yaw = glm::angleAxis(glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::quat pitch = glm::angleAxis(glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::quat roll = glm::angleAxis(glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        return yaw * pitch * roll;
    }

    glm::vec3 Transform::GetForward() const noexcept {
        return glm::normalize(GetRotation() * glm::vec3(0.0f, 0.0f, -1.0f));
    }

    LightComponent LightComponent::CreateDefault(const LightType type) noexcept {
        LightComponent light;
        light.type = type;

        switch (type) {
            case LightType::Directional: light.intensity = 3.0f; break;
            case LightType::Point:
                light.intensity = 20.0f;
                light.range = 10.0f;
                break;
            case LightType::Spot:
                light.intensity = 60.0f;
                light.range = 15.0f;
                light.innerConeAngle = 20.0f;
                light.outerConeAngle = 30.0f;
                break;
        }

        return light;
    }

    glm::vec3 LightComponent::GetRadiance() const noexcept {
        const glm::vec3 tint = useTemperature ? color * ColorTemperatureToLinearRgb(temperature) : color;
        return tint * std::max(intensity, 0.0f);
    }

    Light LightComponent::ToRenderLight(const Transform& transform) const noexcept {
        // Sanitized here rather than trusted: values come from UI fields and level files.
        const float outer = std::clamp(outerConeAngle, 0.1f, 89.9f);
        const float inner = std::clamp(innerConeAngle, 0.0f, outer);

        return Light{
                .type = type,
                .position = transform.position,
                .direction = transform.GetForward(),
                .color = GetRadiance(),
                .range = std::max(range, 0.01f),
                .innerConeCos = std::cos(glm::radians(inner)),
                .outerConeCos = std::cos(glm::radians(outer)),
        };
    }

    const char* GetLightTypeName(const LightType type) noexcept {
        switch (type) {
            case LightType::Directional: return "directional";
            case LightType::Point: return "point";
            case LightType::Spot: return "spot";
        }
        return "unknown";
    }

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

    GameObject::GameObject(const ObjectId id, std::string name) : m_Id(id), m_Name(std::move(name)) {}

    ObjectId GameObject::GetId() const noexcept { return m_Id; }

    const std::string& GameObject::GetName() const noexcept { return m_Name; }

    void GameObject::SetName(std::string name) { m_Name = std::move(name); }

    Transform& GameObject::GetTransform() noexcept { return m_Transform; }

    const Transform& GameObject::GetTransform() const noexcept { return m_Transform; }

    std::optional<LightComponent>& GameObject::GetLight() noexcept { return m_Light; }

    const std::optional<LightComponent>& GameObject::GetLight() const noexcept { return m_Light; }

    void GameObject::SetMesh(std::shared_ptr<const MeshAsset> asset) {
        m_Mesh = std::move(asset);
        m_MeshReference = m_Mesh != nullptr ? AssetReference{.id = m_Mesh->id, .path = m_Mesh->path} : AssetReference{};

        // One override slot per sub-mesh of the new model; overrides for indices it also has are
        // kept (swapping a model for a re-exported version of itself keeps its materials).
        m_MaterialOverrides.resize(m_Mesh != nullptr ? m_Mesh->mesh->GetSubMeshes().size() : 0);
        m_MaterialsDirty = true;
    }

    void GameObject::SetUnresolvedMesh(AssetReference reference) {
        m_Mesh.reset();
        m_MeshReference = std::move(reference);
        m_MaterialsDirty = true;
    }

    const std::shared_ptr<const MeshAsset>& GameObject::GetMesh() const noexcept { return m_Mesh; }

    const AssetReference& GameObject::GetMeshReference() const noexcept { return m_MeshReference; }

    std::optional<Bounds> GameObject::GetWorldBounds() const {
        if (m_Mesh == nullptr) { return std::nullopt; }
        return m_Mesh->bounds.Transformed(m_Transform.ToMatrix());
    }

    std::size_t GameObject::GetMaterialSlotCount() const noexcept { return m_MaterialOverrides.size(); }

    const std::optional<MaterialOverride>& GameObject::GetMaterialOverride(const std::size_t subMeshIndex) const {
        if (subMeshIndex >= m_MaterialOverrides.size()) {
            throw std::out_of_range("GameObject::GetMaterialOverride: sub-mesh index out of range.");
        }
        return m_MaterialOverrides[subMeshIndex];
    }

    void GameObject::SetMaterialOverride(const std::size_t subMeshIndex,
                                         std::optional<MaterialOverride> materialOverride) {
        if (subMeshIndex >= m_MaterialOverrides.size()) { m_MaterialOverrides.resize(subMeshIndex + 1); }

        m_MaterialOverrides[subMeshIndex] = std::move(materialOverride);
        m_MaterialsDirty = true;
    }

    void GameObject::InvalidateMaterials() noexcept { m_MaterialsDirty = true; }

    const std::vector<std::shared_ptr<Material>>& GameObject::GetResolvedMaterials(AssetManager& assets) {
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
                            std::cerr << "[Frost::GameObject] '" << m_Name << "': could not load the "
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
