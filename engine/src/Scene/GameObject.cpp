#include <Frost/Scene/GameObject.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Scene/Hierarchy.hpp>

#include <glm/common.hpp>

#include <utility>

namespace Frost {

    GameObject::GameObject(const ObjectId id, std::string name, Registry& registry)
        : m_Id(id), m_Name(std::move(name)), m_Registry(&registry) {}

    ObjectId GameObject::GetId() const noexcept { return m_Id; }

    const std::string& GameObject::GetName() const noexcept { return m_Name; }

    void GameObject::SetName(std::string name) { m_Name = std::move(name); }

    TransformComponent& GameObject::GetTransform() { return GetComponent<TransformComponent>(); }

    const TransformComponent& GameObject::GetTransform() const { return GetComponent<TransformComponent>(); }

    std::optional<Bounds> GameObject::GetWorldBounds() const {
        std::optional<Bounds> local;
        const auto include = [&local](const std::optional<Bounds>& bounds) {
            if (!bounds.has_value()) { return; }
            if (!local.has_value()) { local = bounds; }
            else {
                local->min = glm::min(local->min, bounds->min);
                local->max = glm::max(local->max, bounds->max);
            }
        };

        if (const MeshComponent* const mesh = TryGetComponent<MeshComponent>()) { include(mesh->GetLocalBounds()); }
        if (const SkeletalMeshComponent* const mesh = TryGetComponent<SkeletalMeshComponent>()) { include(mesh->GetLocalBounds()); }

        if (!local.has_value()) { return std::nullopt; }
        return local->Transformed(GetWorldMatrix());
    }

    glm::mat4 GameObject::GetWorldMatrix() const { return ComputeWorldMatrix(*m_Registry, m_Id); }

}  // namespace Frost
