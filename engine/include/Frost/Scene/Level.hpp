#pragma once

#include <Frost/Scene/GameObject.hpp>
#include <Frost/Scene/Ray.hpp>
#include <Frost/Scene/Registry.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Frost {

    class AssetManager;
    class Renderer;

    // Level-wide lighting settings.
    struct LevelEnvironment {
        // Flat light added to every surface, standing in for indirect/sky light (which the renderer
        // does not compute yet): linear RGB color times intensity.
        glm::vec3 ambientColor{1.0f};
        float ambientIntensity = 0.12f;
    };

    // Where a ray meets a model, for picking in the editor (see Level::Raycast). Not a physics hit:
    // physics raycasts (Frost/Physics/Raycast.hpp) test colliders and return a RaycastHit.
    struct MeshRaycastHit {
        GameObject* object = nullptr;
        std::size_t subMeshIndex = 0;
        float distance = 0.0f;  // Along the ray, in world units (centimeters).
        glm::vec3 point{0.0f};  // World space.
    };

    // A level: the set of objects that make up one playable space. Shared by the game (which loads
    // and draws levels) and the editor (which also builds and saves them; see LevelSerializer).
    //
    // Each object is an entity whose components live in the level's Registry (see GameObject);
    // systems such as rendering (Submit) and physics (PhysicsSystem) work on the registry.
    //
    // Objects are owned by the level and live at stable addresses until destroyed, so pointers to
    // them stay valid across other objects being created or destroyed.
    class Level final {
    public:
        // What a new level is called until it is named.
        static constexpr const char* kDefaultName = "Untitled";

        explicit Level(std::string name = kDefaultName);

        Level(const Level&) = delete;
        Level& operator=(const Level&) = delete;

        Level(Level&&) = delete;
        Level& operator=(Level&&) = delete;

        [[nodiscard]]
        const std::string& GetName() const noexcept;

        void SetName(std::string name);

        [[nodiscard]]
        LevelEnvironment& GetEnvironment() noexcept;

        [[nodiscard]]
        const LevelEnvironment& GetEnvironment() const noexcept;

        // Adds an object with a fresh id and only a (default, identity) TransformComponent.
        GameObject& CreateObject(std::string name);

        // Adds a copy of `source`, an object of this level (its name and a copy of each of its
        // components), with a fresh id.
        GameObject& DuplicateObject(const GameObject& source);

        // Removes the object with this id, if any, and its components. Pointers/references to it
        // become invalid.
        void DestroyObject(ObjectId id);

        [[nodiscard]]
        GameObject* FindObject(ObjectId id) noexcept;

        [[nodiscard]]
        const std::vector<std::unique_ptr<GameObject>>& GetObjects() const noexcept;

        // The components of the level's objects (entity = ObjectId).
        [[nodiscard]]
        Registry& GetRegistry() noexcept;

        [[nodiscard]]
        const Registry& GetRegistry() const noexcept;

        // Brings the skeletal meshes' poses (see SkeletalMeshComponent::UpdatePose) and what their
        // animations say about the visibility of what is attached to them (see
        // UpdateAttachmentVisibility) up to date, so objects attached to bones are where the bones
        // are now. Submit does it first; call it earlier in a frame to place something by an
        // attached object (a camera) before drawing. Cheap when nothing changed.
        void UpdatePoses();

        // Submits a draw for every shown MeshComponent and SkeletalMeshComponent with a loaded
        // model (see Renderer::Submit / SubmitSkinned, IsShownInHierarchy), placed in the world
        // (attached objects where their parents, and bones, are; see ComputeWorldMatrix) after
        // bringing the poses up to date (UpdatePoses); every enabled LightComponent
        // (Renderer::SubmitLight) and the ambient light. Call once per frame.
        void Submit(Renderer& renderer, AssetManager& assets);

        // The world-space box around every object's model and collider (whatever there is to see or
        // bump into), or nullopt if no object has any.
        [[nodiscard]]
        std::optional<Bounds> GetBounds() const;

        // The nearest object surface the ray hits (triangles of the loaded models, skeletal ones in
        // their current pose, both faces), if any, within maxDistance: mesh picking, for the
        // editor. It tests what is drawn, not colliders; for the physics world, see RaycastClosest
        // and RaycastAll in Frost/Physics/Raycast.hpp.
        [[nodiscard]]
        std::optional<MeshRaycastHit> Raycast(const Ray& ray, float maxDistance = 10.0f * kKilometer) const;

    private:
        friend class LevelSerializer;  // Restores saved ids.

        GameObject& AddObject(ObjectId id, std::string name);

        std::string m_Name;
        LevelEnvironment m_Environment;
        Registry m_Registry;  // Before m_Objects: the objects refer to it.
        std::vector<std::unique_ptr<GameObject>> m_Objects;
        ObjectId m_NextId = 1;
    };

}  // namespace Frost
