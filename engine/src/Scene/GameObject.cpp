#include <Frost/Scene/GameObject.hpp>

#include <Frost/Assets/AssetManager.hpp>

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
        const MeshComponent* const mesh = TryGetComponent<MeshComponent>();
        if (mesh == nullptr || mesh->GetMesh() == nullptr) { return std::nullopt; }
        return mesh->GetMesh()->bounds.Transformed(GetTransform().ToMatrix());
    }

}  // namespace Frost
