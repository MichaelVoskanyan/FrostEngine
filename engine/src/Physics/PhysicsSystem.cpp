#include <Frost/Physics/PhysicsSystem.hpp>

#include <Frost/Scene/Components/BoxColliderComponent.hpp>
#include <Frost/Scene/Components/CapsuleColliderComponent.hpp>
#include <Frost/Scene/Components/RigidBodyComponent.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>
#include <Frost/Scene/Hierarchy.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

    // Frames longer than this are simulated as if they were this long (see PhysicsSystem::Update).
    constexpr float kMaxFrameTime = 0.1f;

    // Masses are kept away from zero (a zero mass would mean an infinitely strong push).
    constexpr float kMinMass = 1.0e-3f;

    // A step is split into sub-steps so that no body moves further in one than this fraction of
    // the thinnest collider's half-thickness: a fast body then cannot skip over a thin obstacle, or
    // sink past its middle and be pushed out of the wrong side.
    constexpr float kMaxTravelPerSubStep = 0.9f;
    // Half-thicknesses below this (flat colliders) are treated as this thick for that purpose.
    constexpr float kMinHalfThickness = 1.0f * Frost::kCentimeter;
    constexpr int kMaxSubSteps = 16;

    // A contact whose normal points up at least this much (cos 45 degrees) is something to stand on.
    constexpr float kGroundNormalY = 0.7071f;

    [[nodiscard]]
    float MinHalfExtent(const Frost::CollisionShape& shape) noexcept {
        return std::max(Frost::GetMinHalfExtent(shape), kMinHalfThickness);
    }

    // Contacts this close count as touching, before a step moves anything; see
    // PhysicsSystem::StopAtTouchingContacts.
    constexpr float kContactMargin = 0.5f * Frost::kCentimeter;

    // Limits `velocity`'s motion into a surface (along -normal) to `allowedApproachSpeed` (0 =
    // none at all), and slows its sliding along the surface by friction in proportion to the speed
    // that was removed. Returns the new velocity.
    [[nodiscard]]
    glm::vec3 StopAgainstSurface(const glm::vec3& velocity, const glm::vec3& normal, const float friction,
                                 const float allowedApproachSpeed) noexcept {
        const float normalSpeed = glm::dot(velocity, normal);
        if (normalSpeed >= -allowedApproachSpeed) { return velocity; }  // Not approaching too fast.

        const float removedSpeed = -allowedApproachSpeed - normalSpeed;
        const glm::vec3 tangent = velocity - normal * normalSpeed;
        const float tangentSpeed = glm::length(tangent);

        glm::vec3 sliding(0.0f);
        if (tangentSpeed > 0.0f) {
            const float slowDown = std::min(friction * removedSpeed, tangentSpeed);
            sliding = tangent * ((tangentSpeed - slowDown) / tangentSpeed);
        }
        return sliding - normal * allowedApproachSpeed;
    }

}  // namespace

namespace Frost {

    PhysicsSystem::PhysicsSystem(const PhysicsSettings& settings) { SetSettings(settings); }

    void PhysicsSystem::Update(Registry& registry, const float deltaTime) {
        if (!(deltaTime > 0.0f)) { return; }  // Also skips NaN.

        m_Accumulator += std::min(deltaTime, kMaxFrameTime);
        while (m_Accumulator >= m_Settings.fixedTimeStep) {
            Step(registry, m_Settings.fixedTimeStep);
            m_Accumulator -= m_Settings.fixedTimeStep;
        }
    }

    bool PhysicsSystem::Body::Collides() const noexcept { return box != nullptr || capsule != nullptr; }

    const CollisionFilter& PhysicsSystem::Body::GetFilter() const noexcept {
        return box != nullptr ? box->filter : capsule->filter;
    }

    CollisionShape PhysicsSystem::Body::GetShape() const noexcept {
        if (box != nullptr) { return MakeWorldBox(*transform, *box); }
        return MakeWorldCapsule(*transform, *capsule);
    }

    void PhysicsSystem::Step(Registry& registry, const float timeStep) {
        if (!(timeStep > 0.0f)) { return; }

        Gather(registry);

        // Found again by this step's contacts.
        for (Body& body : m_Bodies) { body.rigidBody->onGround = false; }

        for (Body& body : m_Bodies) { body.rigidBody->linearVelocity += m_Settings.gravity * timeStep; }

        StopAtTouchingContacts(timeStep);

        const int subSteps = CountSubSteps(timeStep);
        const float subStep = timeStep / static_cast<float>(subSteps);

        for (int i = 0; i < subSteps; ++i) {
            for (Body& body : m_Bodies) { body.transform->position += body.rigidBody->linearVelocity * subStep; }
            ResolveOverlaps();
        }

        // The pointers are only good for this step.
        m_Bodies.clear();
        m_Obstacles.clear();
    }

    void PhysicsSystem::Reset() noexcept { m_Accumulator = 0.0f; }

    const PhysicsSettings& PhysicsSystem::GetSettings() const noexcept { return m_Settings; }

    void PhysicsSystem::SetSettings(const PhysicsSettings& settings) noexcept {
        m_Settings = settings;

        // A step of zero (or negative, or NaN) length would never advance.
        if (!(m_Settings.fixedTimeStep > 0.0f)) { m_Settings.fixedTimeStep = PhysicsSettings{}.fixedTimeStep; }
        m_Settings.solverIterations = std::max(m_Settings.solverIterations, 1);
        m_Settings.friction = std::max(m_Settings.friction, 0.0f);
    }

    void PhysicsSystem::Gather(Registry& registry) {
        m_Bodies.clear();
        m_Obstacles.clear();

        registry.Each<TransformComponent, RigidBodyComponent>(
                [&](const EntityId entity, TransformComponent& transform, RigidBodyComponent& rigidBody) {
                    if (!rigidBody.simulated || IsAttached(registry, entity)) {
                        rigidBody.onGround = false;  // Not simulated: not standing anywhere.
                        return;
                    }
                    const BoxColliderComponent* const box = registry.TryGet<BoxColliderComponent>(entity);
                    m_Bodies.push_back(Body{.transform = &transform,
                                            .rigidBody = &rigidBody,
                                            .box = box,
                                            .capsule = box == nullptr ? registry.TryGet<CapsuleColliderComponent>(entity)
                                                                      : nullptr,
                                            .inverseMass = 1.0f / std::max(rigidBody.mass, kMinMass)});
                });

        // Everything else that collides is an obstacle.
        const auto isBody = [&registry](const EntityId entity) {
            const RigidBodyComponent* const rigidBody = registry.TryGet<RigidBodyComponent>(entity);
            return rigidBody != nullptr && rigidBody->simulated;
        };
        registry.Each<TransformComponent, BoxColliderComponent>(
                [&](const EntityId entity, const TransformComponent& transform, const BoxColliderComponent& collider) {
                    if (!isBody(entity) && !IsAttached(registry, entity)) {
                        m_Obstacles.push_back(Obstacle{.shape = MakeWorldBox(transform, collider), .filter = collider.filter});
                    }
                });
        registry.Each<TransformComponent, CapsuleColliderComponent>(
                [&](const EntityId entity, const TransformComponent& transform,
                    const CapsuleColliderComponent& collider) {
                    // One collider per object: a box, if it also has one, was taken above.
                    if (!isBody(entity) && !registry.Has<BoxColliderComponent>(entity) && !IsAttached(registry, entity)) {
                        m_Obstacles.push_back(Obstacle{.shape = MakeWorldCapsule(transform, collider), .filter = collider.filter});
                    }
                });
    }

    int PhysicsSystem::CountSubSteps(const float timeStep) const {
        float thinnest = std::numeric_limits<float>::max();
        float fastest = 0.0f;

        for (const Obstacle& obstacle : m_Obstacles) { thinnest = std::min(thinnest, MinHalfExtent(obstacle.shape)); }
        for (const Body& body : m_Bodies) {
            if (!body.Collides()) { continue; }  // Collides with nothing, so cannot tunnel.
            thinnest = std::min(thinnest, MinHalfExtent(body.GetShape()));
            fastest = std::max(fastest, glm::length(body.rigidBody->linearVelocity));
        }

        if (fastest <= 0.0f || thinnest == std::numeric_limits<float>::max()) { return 1; }

        const float subSteps = std::ceil(fastest * timeStep / (thinnest * kMaxTravelPerSubStep));
        if (!(subSteps > 1.0f)) { return 1; }  // Also NaN.
        return subSteps >= static_cast<float>(kMaxSubSteps) ? kMaxSubSteps : static_cast<int>(subSteps);
    }

    // Velocities are settled against whatever each body already touches (or nearly touches) before
    // anything moves. A body resting on a surface therefore does not move at all, instead of
    // sinking in under gravity and being pushed back out along the surface's normal, which on a
    // slope would make it creep downhill however much friction there is. A body a little above a
    // surface may still close the gap within this step (a speculative contact), so it lands
    // exactly rather than hovering.
    void PhysicsSystem::StopAtTouchingContacts(const float timeStep) {
        const auto inflated = [](const Body& body) { return Inflate(body.GetShape(), kContactMargin); };
        // The gap left between the real boxes, from the contact of the inflated one.
        const auto approachSpeed = [&](const Contact& contact) {
            return std::max(kContactMargin - contact.depth, 0.0f) / timeStep;
        };

        for (int iteration = 0; iteration < m_Settings.solverIterations; ++iteration) {
            for (std::size_t i = 0; i < m_Bodies.size(); ++i) {
                Body& body = m_Bodies[i];
                if (!body.Collides()) { continue; }
                const CollisionShape shape = inflated(body);
                const CollisionFilter& filter = body.GetFilter();

                for (const Obstacle& obstacle : m_Obstacles) {
                    if (!filter.CollidesWith(obstacle.filter)) { continue; }
                    if (const std::optional<Contact> contact = FindContact(shape, obstacle.shape)) {
                        StopBody(body, contact->normal, approachSpeed(*contact));
                    }
                }

                for (std::size_t j = i + 1; j < m_Bodies.size(); ++j) {
                    Body& other = m_Bodies[j];
                    if (!other.Collides() || !filter.CollidesWith(other.GetFilter())) { continue; }

                    if (const std::optional<Contact> contact = FindContact(shape, other.GetShape())) {
                        StopPair(body, other, contact->normal, approachSpeed(*contact));
                    }
                }
            }
        }
    }

    void PhysicsSystem::ResolveOverlaps() {
        for (int iteration = 0; iteration < m_Settings.solverIterations; ++iteration) {
            bool anyContact = false;

            for (std::size_t i = 0; i < m_Bodies.size(); ++i) {
                Body& body = m_Bodies[i];
                if (!body.Collides()) { continue; }
                const CollisionFilter& filter = body.GetFilter();

                for (const Obstacle& obstacle : m_Obstacles) {
                    if (!filter.CollidesWith(obstacle.filter)) { continue; }
                    // Rebuilt per test: the previous contact may have moved the body.
                    const std::optional<Contact> contact = FindContact(body.GetShape(), obstacle.shape);
                    if (!contact.has_value()) { continue; }

                    body.transform->position += contact->normal * contact->depth;
                    StopBody(body, contact->normal, 0.0f);
                    anyContact = true;
                }

                for (std::size_t j = i + 1; j < m_Bodies.size(); ++j) {
                    Body& other = m_Bodies[j];
                    if (!other.Collides() || !filter.CollidesWith(other.GetFilter())) { continue; }

                    const std::optional<Contact> contact = FindContact(body.GetShape(), other.GetShape());
                    if (!contact.has_value()) { continue; }

                    // Each is pushed in proportion to its share of the inverse mass: the lighter one
                    // moves more.
                    const float totalInverseMass = body.inverseMass + other.inverseMass;
                    body.transform->position += contact->normal * (contact->depth * body.inverseMass / totalInverseMass);
                    other.transform->position -= contact->normal * (contact->depth * other.inverseMass / totalInverseMass);
                    StopPair(body, other, contact->normal, 0.0f);
                    anyContact = true;
                }
            }

            if (!anyContact) { return; }  // Everything is apart; further passes would change nothing.
        }
    }

    void PhysicsSystem::StopBody(Body& body, const glm::vec3& normal, const float allowedApproachSpeed) const {
        if (normal.y >= kGroundNormalY) { body.rigidBody->onGround = true; }
        body.rigidBody->linearVelocity =
                StopAgainstSurface(body.rigidBody->linearVelocity, normal, m_Settings.friction, allowedApproachSpeed);
    }

    void PhysicsSystem::StopPair(Body& first, Body& second, const glm::vec3& normal, const float allowedApproachSpeed) const {
        // Standing on each other: the normal points from `second` towards `first`.
        if (normal.y >= kGroundNormalY) { first.rigidBody->onGround = true; }
        if (-normal.y >= kGroundNormalY) { second.rigidBody->onGround = true; }

        // On the relative motion: the change is shared in proportion to inverse mass, which
        // conserves momentum (the lighter body's velocity changes more).
        glm::vec3& firstVelocity = first.rigidBody->linearVelocity;
        glm::vec3& secondVelocity = second.rigidBody->linearVelocity;

        const glm::vec3 relative = firstVelocity - secondVelocity;
        const glm::vec3 change =
                StopAgainstSurface(relative, normal, m_Settings.friction, allowedApproachSpeed) - relative;

        const float totalInverseMass = first.inverseMass + second.inverseMass;
        firstVelocity += change * (first.inverseMass / totalInverseMass);
        secondVelocity -= change * (second.inverseMass / totalInverseMass);
    }

}  // namespace Frost
