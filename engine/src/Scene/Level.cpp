#include <Frost/Scene/Level.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Physics/Collision.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Renderer/Renderer.hpp>
#include <Frost/Scene/Hierarchy.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

    // Slab test: the entry distance of the ray into the box, if it hits within [0, maxDistance].
    // `inverseDirection` components may be infinite (axis-parallel rays), which the min/max
    // arithmetic handles.
    std::optional<float> IntersectBox(const glm::vec3& origin, const glm::vec3& inverseDirection,
                                      const Frost::Bounds& box, const float maxDistance) {
        const glm::vec3 t0 = (box.min - origin) * inverseDirection;
        const glm::vec3 t1 = (box.max - origin) * inverseDirection;

        const glm::vec3 tNear = glm::min(t0, t1);
        const glm::vec3 tFar = glm::max(t0, t1);

        const float entry = std::max({tNear.x, tNear.y, tNear.z, 0.0f});
        const float exit = std::min({tFar.x, tFar.y, tFar.z, maxDistance});

        // NaN (0 * inf, a ray lying exactly in a slab plane) fails both comparisons and counts as
        // a miss, which is fine for picking.
        if (!(entry <= exit)) { return std::nullopt; }
        return entry;
    }

    // Moller-Trumbore, both faces. Returns the ray parameter of the hit, if it is within
    // (epsilon, maxDistance).
    std::optional<float> IntersectTriangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a,
                                           const glm::vec3& b, const glm::vec3& c, const float maxDistance) {
        constexpr float kEpsilon = 1.0e-7f;

        const glm::vec3 edge1 = b - a;
        const glm::vec3 edge2 = c - a;
        const glm::vec3 p = glm::cross(direction, edge2);
        const float determinant = glm::dot(edge1, p);

        if (std::abs(determinant) < kEpsilon) { return std::nullopt; }  // Parallel to the triangle.

        const float inverseDeterminant = 1.0f / determinant;
        const glm::vec3 s = origin - a;

        const float u = glm::dot(s, p) * inverseDeterminant;
        if (u < 0.0f || u > 1.0f) { return std::nullopt; }

        const glm::vec3 q = glm::cross(s, edge1);
        const float v = glm::dot(direction, q) * inverseDeterminant;
        if (v < 0.0f || u + v > 1.0f) { return std::nullopt; }

        const float t = glm::dot(edge2, q) * inverseDeterminant;
        if (t <= kEpsilon || t >= maxDistance) { return std::nullopt; }
        return t;
    }

}  // namespace

namespace Frost {

    Level::Level(std::string name) : m_Name(std::move(name)) {}

    const std::string& Level::GetName() const noexcept { return m_Name; }

    void Level::SetName(std::string name) { m_Name = std::move(name); }

    LevelEnvironment& Level::GetEnvironment() noexcept { return m_Environment; }

    const LevelEnvironment& Level::GetEnvironment() const noexcept { return m_Environment; }

    GameObject& Level::CreateObject(std::string name) { return AddObject(m_NextId, std::move(name)); }

    GameObject& Level::DuplicateObject(const GameObject& source) {
        if (FindObject(source.GetId()) != &source) {
            throw std::invalid_argument("Level::DuplicateObject: the object is not part of this level.");
        }

        GameObject& copy = CreateObject(source.GetName());
        m_Registry.CopyAll(source.GetId(), copy.GetId());
        return copy;
    }

    void Level::DestroyObject(const ObjectId id) {
        // What was attached to it is left where it is, no longer attached.
        for (const EntityId child : GetAttachedObjects(m_Registry, id)) {
            if (child == id) { continue; }
            const glm::mat4 world = ComputeWorldMatrix(m_Registry, child);
            m_Registry.Get<AttachmentComponent>(child).parent = 0;
            m_Registry.Get<TransformComponent>(child).SetFromMatrix(world);
        }

        m_Registry.RemoveAll(id);
        std::erase_if(m_Objects, [id](const std::unique_ptr<GameObject>& object) { return object->GetId() == id; });
    }

    GameObject* Level::FindObject(const ObjectId id) noexcept {
        for (const std::unique_ptr<GameObject>& object : m_Objects) {
            if (object->GetId() == id) { return object.get(); }
        }
        return nullptr;
    }

    const std::vector<std::unique_ptr<GameObject>>& Level::GetObjects() const noexcept { return m_Objects; }

    Registry& Level::GetRegistry() noexcept { return m_Registry; }

    const Registry& Level::GetRegistry() const noexcept { return m_Registry; }

    void Level::UpdatePoses() {
        m_Registry.Each<SkeletalMeshComponent>([](EntityId, SkeletalMeshComponent& mesh) { mesh.UpdatePose(); });
        UpdateAttachmentVisibility(m_Registry);
    }

    void Level::Submit(Renderer& renderer, AssetManager& assets) {
        renderer.SetAmbientLight(m_Environment.ambientColor * std::max(m_Environment.ambientIntensity, 0.0f));

        // Poses first: what is attached to a bone goes where the bone is now, and is shown or hidden
        // as the animation posing it says.
        UpdatePoses();

        m_Registry.Each<TransformComponent, LightComponent>([&](const EntityId object, const TransformComponent&, const LightComponent& light) {
            if (!light.enabled) { return; }
            const WorldPlacement placement = ComputeWorldPlacement(m_Registry, object);
            renderer.SubmitLight(light.ToRenderLight(placement.position, placement.rotation));
        });

        m_Registry.Each<TransformComponent, MeshComponent>([&](const EntityId object, const TransformComponent&, MeshComponent& mesh) {
            if (mesh.GetMesh() != nullptr && IsShownInHierarchy(m_Registry, object)) {
                renderer.Submit(mesh.GetMesh()->mesh, ComputeWorldMatrix(m_Registry, object), mesh.GetResolvedMaterials(assets));
            }
        });

        m_Registry.Each<TransformComponent, SkeletalMeshComponent>(
                [&](const EntityId object, const TransformComponent&, SkeletalMeshComponent& mesh) {
                    if (mesh.GetDrawable() != nullptr && IsShownInHierarchy(m_Registry, object)) {
                        renderer.SubmitSkinned(mesh.GetDrawable()->mesh, ComputeWorldMatrix(m_Registry, object),
                                               mesh.GetResolvedMaterials(assets), mesh.GetPalette());
                    }
                });
    }

    std::optional<Bounds> Level::GetBounds() const {
        std::optional<Bounds> all;
        const auto include = [&all](const Bounds& bounds) {
            if (!all.has_value()) { all = bounds; }
            else {
                all->min = glm::min(all->min, bounds.min);
                all->max = glm::max(all->max, bounds.max);
            }
        };

        for (const std::unique_ptr<GameObject>& object : m_Objects) {
            if (const std::optional<Bounds> model = object->GetWorldBounds()) { include(*model); }
            if (IsAttached(m_Registry, object->GetId())) { continue; }  // Colliders of attached objects do not collide.
            if (const BoxColliderComponent* const collider = object->TryGetComponent<BoxColliderComponent>()) {
                include(MakeWorldBox(object->GetTransform(), *collider).GetBounds());
            }
            if (const CapsuleColliderComponent* const collider = object->TryGetComponent<CapsuleColliderComponent>()) {
                include(MakeWorldCapsule(object->GetTransform(), *collider).GetBounds());
            }
        }
        return all;
    }

    std::optional<MeshRaycastHit> Level::Raycast(const Ray& ray, const float maxDistance) const {
        std::optional<MeshRaycastHit> nearest;
        float nearestDistance = maxDistance;

        // Tests one model an object draws (its vertices at `positions`, boxed by `bounds`), keeping
        // the nearest hit.
        const auto testModel = [&](GameObject& object, const MeshAsset& asset, const std::vector<glm::vec3>& positions,
                                   const Bounds& bounds) {
            // Test in the object's own space. The direction is transformed but deliberately not
            // renormalized, so the ray parameter t measures the same world-space distance on both
            // sides of the transform (and a non-uniform scale is handled exactly).
            const glm::mat4 model = object.GetWorldMatrix();
            if (glm::determinant(glm::mat3(model)) == 0.0f) { return; }  // Zero scale: nothing to hit.

            const glm::mat4 inverse = glm::inverse(model);
            const glm::vec3 origin = glm::vec3(inverse * glm::vec4(ray.origin, 1.0f));
            const glm::vec3 direction = glm::vec3(inverse * glm::vec4(ray.direction, 0.0f));
            const glm::vec3 inverseDirection = 1.0f / direction;

            if (!IntersectBox(origin, inverseDirection, bounds, nearestDistance).has_value()) { return; }

            const std::vector<SubMesh>& subMeshes = asset.mesh->GetSubMeshes();
            for (std::size_t s = 0; s < subMeshes.size(); ++s) {
                const SubMesh& subMesh = subMeshes[s];

                for (std::uint32_t i = 0; i + 2 < subMesh.indexCount; i += 3) {
                    const std::uint32_t first = subMesh.firstIndex + i;
                    const auto vertex = [&](const std::uint32_t index) -> const glm::vec3& {
                        return positions[asset.indices[index] + static_cast<std::uint32_t>(subMesh.vertexOffset)];
                    };

                    const std::optional<float> t =
                            IntersectTriangle(origin, direction, vertex(first), vertex(first + 1), vertex(first + 2),
                                              nearestDistance);
                    if (!t.has_value()) { continue; }

                    nearestDistance = *t;
                    nearest = MeshRaycastHit{.object = &object,
                                         .subMeshIndex = s,
                                         .distance = *t,
                                         .point = ray.origin + ray.direction * *t};
                }
            }
        };

        // Every model an object draws: its mesh, and its skeletal mesh in its current pose (hidden
        // ones cannot be hit).
        for (const std::unique_ptr<GameObject>& object : m_Objects) {
            if (!IsShownInHierarchy(m_Registry, object->GetId())) { continue; }
            if (const MeshComponent* const mesh = object->TryGetComponent<MeshComponent>(); mesh != nullptr && mesh->GetMesh() != nullptr) {
                testModel(*object, *mesh->GetMesh(), mesh->GetMesh()->positions, mesh->GetMesh()->bounds);
            }
            if (const SkeletalMeshComponent* const mesh = object->TryGetComponent<SkeletalMeshComponent>();
                mesh != nullptr && mesh->GetDrawable() != nullptr) {
                const std::vector<glm::vec3>& positions = mesh->GetPosedPositions();
                const std::optional<Bounds> bounds = mesh->GetLocalBounds();
                if (bounds.has_value() && positions.size() == mesh->GetDrawable()->positions.size()) {
                    testModel(*object, *mesh->GetDrawable(), positions, *bounds);
                }
            }
        }

        return nearest;
    }

    GameObject& Level::AddObject(const ObjectId id, std::string name) {
        m_Objects.push_back(std::make_unique<GameObject>(id, std::move(name), m_Registry));
        m_NextId = std::max(m_NextId, id + 1);

        // Every object is somewhere.
        m_Registry.Add<TransformComponent>(id);
        return *m_Objects.back();
    }

}  // namespace Frost
