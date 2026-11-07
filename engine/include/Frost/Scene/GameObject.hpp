#pragma once

#include <Frost/Scene/Bounds.hpp>
#include <Frost/Scene/Components/Components.hpp>
#include <Frost/Scene/Registry.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace Frost {

    // Unique (within a level) and stable (saved with the level) object identifier. 0 is never a
    // valid id. An object's id is also its entity id in the level's Registry.
    using ObjectId = EntityId;

    // An object placed in a level: an entity of the level's entity component system, with a name.
    // Everything else about it is in its components (see Frost/Scene/Components/): every object
    // has a TransformComponent, and may have others, e.g. a MeshComponent (a model to draw), a
    // LightComponent, a BoxColliderComponent and a RigidBodyComponent.
    //
    // Objects are created and owned by a Level, which also holds their components (in its
    // Registry); a GameObject is a handle onto them with a stable address. Component references
    // are invalidated when a component of the same type is added to or removed from any object of
    // the level, so look components up again rather than keeping them.
    class GameObject final {
    public:
        // Use Level::CreateObject; this attaches nothing (Level gives the object its transform).
        GameObject(ObjectId id, std::string name, Registry& registry);

        GameObject(const GameObject&) = delete;
        GameObject& operator=(const GameObject&) = delete;

        GameObject(GameObject&&) = delete;
        GameObject& operator=(GameObject&&) = delete;

        [[nodiscard]]
        ObjectId GetId() const noexcept;

        [[nodiscard]]
        const std::string& GetName() const noexcept;

        void SetName(std::string name);

        // --- Components ----------------------------------------------------------------------------

        // Attaches a component built from `args` (replacing one of the same type the object already
        // has) and returns it.
        template <Component T, typename... Args>
        T& AddComponent(Args&&... args) {
            return m_Registry->Add<T>(m_Id, std::forward<Args>(args)...);
        }

        // Detaches a component (if the object has one). The transform cannot be removed.
        template <Component T>
        void RemoveComponent() {
            static_assert(!std::is_same_v<T, TransformComponent>, "Every object keeps its TransformComponent.");
            m_Registry->Remove<T>(m_Id);
        }

        template <Component T>
        [[nodiscard]]
        bool HasComponent() const noexcept {
            return m_Registry->Has<T>(m_Id);
        }

        // The object's component; throws std::out_of_range if it has none.
        template <Component T>
        [[nodiscard]]
        T& GetComponent() {
            return m_Registry->Get<T>(m_Id);
        }

        template <Component T>
        [[nodiscard]]
        const T& GetComponent() const {
            return std::as_const(*m_Registry).Get<T>(m_Id);
        }

        // The object's component, or null if it has none.
        template <Component T>
        [[nodiscard]]
        T* TryGetComponent() noexcept {
            return m_Registry->TryGet<T>(m_Id);
        }

        template <Component T>
        [[nodiscard]]
        const T* TryGetComponent() const noexcept {
            return std::as_const(*m_Registry).TryGet<T>(m_Id);
        }

        // Shorthand for GetComponent<TransformComponent>(), which every object has.
        [[nodiscard]]
        TransformComponent& GetTransform();

        [[nodiscard]]
        const TransformComponent& GetTransform() const;

        // The transform from the object's space to the world: its own transform, on top of the
        // object it is attached to if it is (see AttachmentComponent, ComputeWorldMatrix).
        [[nodiscard]]
        glm::mat4 GetWorldMatrix() const;

        // World-space box around the object's model(s) (a skeletal mesh in its current pose), or
        // nullopt if it has no model loaded.
        [[nodiscard]]
        std::optional<Bounds> GetWorldBounds() const;

    private:
        ObjectId m_Id;
        std::string m_Name;
        Registry* m_Registry;  // The owning level's; the level outlives its objects.
    };

}  // namespace Frost
