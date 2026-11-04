#pragma once

#include <Frost/Scene/GameObject.hpp>

#include <glm/vec3.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Frost {

    class AssetManager;
    class Renderer;

    struct Ray {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};  // Unit length.
    };

    // Level-wide lighting settings.
    struct LevelEnvironment {
        // Flat light added to every surface, standing in for indirect/sky light (which the renderer
        // does not compute yet): linear RGB color times intensity.
        glm::vec3 ambientColor{1.0f};
        float ambientIntensity = 0.12f;
    };

    struct RaycastHit {
        GameObject* object = nullptr;
        std::size_t subMeshIndex = 0;
        float distance = 0.0f;  // Along the ray, in world units.
        glm::vec3 point{0.0f};  // World space.
    };

    // A level: the set of objects that make up one playable space. Shared by the game (which loads
    // and draws levels) and the editor (which also builds and saves them; see LevelSerializer).
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

        // Adds an empty object (no model, identity transform) with a fresh id.
        GameObject& CreateObject(std::string name);

        // Adds a copy of `source` (name, transform, model, overrides and light) with a fresh id.
        GameObject& DuplicateObject(const GameObject& source);

        // Removes the object with this id, if any. Pointers/references to it become invalid.
        void DestroyObject(ObjectId id);

        [[nodiscard]]
        GameObject* FindObject(ObjectId id) noexcept;

        [[nodiscard]]
        const std::vector<std::unique_ptr<GameObject>>& GetObjects() const noexcept;

        // Submits a draw for every object that has a model (see Renderer::Submit), every enabled
        // light (Renderer::SubmitLight) and the ambient light. Call once per frame.
        void Submit(Renderer& renderer, AssetManager& assets);

        // The nearest object surface the ray hits (triangles of the loaded models, both faces),
        // if any, within maxDistance.
        [[nodiscard]]
        std::optional<RaycastHit> Raycast(const Ray& ray, float maxDistance = 1.0e6f) const;

    private:
        friend class LevelSerializer;  // Restores saved ids.

        GameObject& AddObject(ObjectId id, std::string name);

        std::string m_Name;
        LevelEnvironment m_Environment;
        std::vector<std::unique_ptr<GameObject>> m_Objects;
        ObjectId m_NextId = 1;
    };

}  // namespace Frost
