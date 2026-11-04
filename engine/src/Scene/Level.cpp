#include <Frost/Scene/Level.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Renderer/Renderer.hpp>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
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
        GameObject& copy = CreateObject(source.GetName());
        copy.GetTransform() = source.GetTransform();
        copy.GetLight() = source.GetLight();

        if (source.GetMesh() != nullptr) { copy.SetMesh(source.GetMesh()); }
        else if (source.GetMeshReference().IsSet()) { copy.SetUnresolvedMesh(source.GetMeshReference()); }

        for (std::size_t i = 0; i < source.GetMaterialSlotCount(); ++i) {
            copy.SetMaterialOverride(i, source.GetMaterialOverride(i));
        }

        return copy;
    }

    void Level::DestroyObject(const ObjectId id) {
        std::erase_if(m_Objects, [id](const std::unique_ptr<GameObject>& object) { return object->GetId() == id; });
    }

    GameObject* Level::FindObject(const ObjectId id) noexcept {
        for (const std::unique_ptr<GameObject>& object : m_Objects) {
            if (object->GetId() == id) { return object.get(); }
        }
        return nullptr;
    }

    const std::vector<std::unique_ptr<GameObject>>& Level::GetObjects() const noexcept { return m_Objects; }

    void Level::Submit(Renderer& renderer, AssetManager& assets) {
        renderer.SetAmbientLight(m_Environment.ambientColor * std::max(m_Environment.ambientIntensity, 0.0f));

        for (const std::unique_ptr<GameObject>& object : m_Objects) {
            if (const std::optional<LightComponent>& light = object->GetLight(); light.has_value() && light->enabled) {
                renderer.SubmitLight(light->ToRenderLight(object->GetTransform()));
            }

            if (object->GetMesh() != nullptr) {
                renderer.Submit(object->GetMesh()->mesh, object->GetTransform().ToMatrix(),
                                object->GetResolvedMaterials(assets));
            }
        }
    }

    std::optional<RaycastHit> Level::Raycast(const Ray& ray, const float maxDistance) const {
        std::optional<RaycastHit> nearest;
        float nearestDistance = maxDistance;

        for (const std::unique_ptr<GameObject>& object : m_Objects) {
            const std::shared_ptr<const MeshAsset>& asset = object->GetMesh();
            if (asset == nullptr) { continue; }

            // Test in the object's own space. The direction is transformed but deliberately not
            // renormalized, so the ray parameter t measures the same world-space distance on both
            // sides of the transform (and a non-uniform scale is handled exactly).
            const glm::mat4 model = object->GetTransform().ToMatrix();
            if (glm::determinant(glm::mat3(model)) == 0.0f) { continue; }  // Zero scale: nothing to hit.

            const glm::mat4 inverse = glm::inverse(model);
            const glm::vec3 origin = glm::vec3(inverse * glm::vec4(ray.origin, 1.0f));
            const glm::vec3 direction = glm::vec3(inverse * glm::vec4(ray.direction, 0.0f));
            const glm::vec3 inverseDirection = 1.0f / direction;

            if (!IntersectBox(origin, inverseDirection, asset->bounds, nearestDistance).has_value()) { continue; }

            const std::vector<SubMesh>& subMeshes = asset->mesh->GetSubMeshes();
            for (std::size_t s = 0; s < subMeshes.size(); ++s) {
                const SubMesh& subMesh = subMeshes[s];

                for (std::uint32_t i = 0; i + 2 < subMesh.indexCount; i += 3) {
                    const std::uint32_t first = subMesh.firstIndex + i;
                    const auto vertex = [&](const std::uint32_t index) -> const glm::vec3& {
                        return asset->positions[asset->indices[index] + static_cast<std::uint32_t>(subMesh.vertexOffset)];
                    };

                    const std::optional<float> t =
                            IntersectTriangle(origin, direction, vertex(first), vertex(first + 1), vertex(first + 2),
                                              nearestDistance);
                    if (!t.has_value()) { continue; }

                    nearestDistance = *t;
                    nearest = RaycastHit{.object = object.get(),
                                         .subMeshIndex = s,
                                         .distance = *t,
                                         .point = ray.origin + ray.direction * *t};
                }
            }
        }

        return nearest;
    }

    GameObject& Level::AddObject(const ObjectId id, std::string name) {
        m_Objects.push_back(std::make_unique<GameObject>(id, std::move(name)));
        m_NextId = std::max(m_NextId, id + 1);
        return *m_Objects.back();
    }

}  // namespace Frost
