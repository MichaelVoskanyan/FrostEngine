#include "PhysicsBenchmark.hpp"

#include <Frost/Physics/Overlap.hpp>
#include <Frost/Physics/PhysicsQuerySystem.hpp>
#include <Frost/Physics/Raycast.hpp>
#include <Frost/Physics/ShapeCast.hpp>
#include <Frost/Scene/Level.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <ostream>
#include <random>
#include <string>
#include <utility>

namespace {

    using Clock = std::chrono::steady_clock;

    // Each collider gets, on average, this much ground to itself (a square this wide): the scene
    // grows with the number of colliders, so they stay about as dense.
    constexpr float kGroundPerCollider = 4.0f * Frost::kMeter;
    constexpr float kMinAreaSize = 20.0f * Frost::kMeter;

    // Collider sizes.
    constexpr float kMinBoxSize = 50.0f * Frost::kCentimeter;
    constexpr float kMaxBoxSize = 3.0f * Frost::kMeter;
    constexpr float kMinCapsuleRadius = 20.0f * Frost::kCentimeter;
    constexpr float kMaxCapsuleRadius = 1.0f * Frost::kMeter;
    constexpr float kMaxCapsuleHeight = 4.0f * Frost::kMeter;

    // The queries: casts reach this far; their shapes and the overlap volumes are about as big as a
    // thrown object, a character, and an explosion.
    constexpr float kReach = 50.0f * Frost::kMeter;
    constexpr float kCastSphereRadius = 50.0f * Frost::kCentimeter;
    constexpr float kCastCapsuleRadius = 40.0f * Frost::kCentimeter;
    constexpr float kCastCapsuleHeight = 1.8f * Frost::kMeter;
    constexpr float kOverlapSphereRadius = 3.0f * Frost::kMeter;
    constexpr float kOverlapBoxHalfSize = 2.0f * Frost::kMeter;

    constexpr const char* kPrefix = "[Benchmark] ";

    // Random numbers made straight from std::mt19937's output, which the standard defines exactly:
    // the standard distributions are not (MSVC and libstdc++ give different numbers), so a seed
    // gives the same scene on every platform.
    class Random final {
    public:
        explicit Random(const std::uint32_t seed) : m_Engine(seed) {}

        // In [low, high).
        float Uniform(const float low, const float high) {
            return low + (high - low) * (static_cast<float>(m_Engine() >> 8) * (1.0f / 16777216.0f));
        }

        // A unit vector, any direction equally likely.
        glm::vec3 Direction() {
            const float z = Uniform(-1.0f, 1.0f);
            const float angle = Uniform(0.0f, glm::two_pi<float>());
            const float across = std::sqrt(std::max(1.0f - z * z, 0.0f));
            return glm::vec3(across * std::cos(angle), across * std::sin(angle), z);
        }

        // Any orientation equally likely (Shoemake's uniform random quaternion).
        glm::quat Rotation() {
            const float u1 = Uniform(0.0f, 1.0f);
            const float u2 = Uniform(0.0f, glm::two_pi<float>());
            const float u3 = Uniform(0.0f, glm::two_pi<float>());
            const float a = std::sqrt(1.0f - u1);
            const float b = std::sqrt(u1);
            return glm::normalize(glm::quat(b * std::cos(u3), a * std::sin(u2), a * std::cos(u2), b * std::sin(u3)));
        }

    private:
        std::mt19937 m_Engine;
    };

    // Euler angles (degrees, as TransformComponent stores them) for a rotation.
    [[nodiscard]]
    glm::vec3 ToEulerDegrees(const glm::quat& rotation) {
        Frost::TransformComponent transform;
        transform.SetFromMatrix(glm::mat4_cast(rotation));
        return transform.rotation;
    }

    // A point anywhere in the scene's volume.
    [[nodiscard]]
    glm::vec3 RandomPosition(Random& random, const float areaSize) {
        const float half = areaSize * 0.5f;
        return glm::vec3(random.Uniform(-half, half), random.Uniform(0.0f, kBenchmarkAreaHeight), random.Uniform(-half, half));
    }

    [[nodiscard]]
    Frost::Capsule MakeCastCapsule(const glm::vec3& center) {
        // Standing up, like a character.
        const float halfSegment = kCastCapsuleHeight * 0.5f - kCastCapsuleRadius;
        return Frost::Capsule{.start = center - glm::vec3(0.0f, halfSegment, 0.0f),
                              .end = center + glm::vec3(0.0f, halfSegment, 0.0f),
                              .radius = kCastCapsuleRadius};
    }

    [[nodiscard]]
    double MicrosecondsSince(const Clock::time_point start) {
        return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    }

    // One kind of query: runs it for every query of the batch, and gives the number of colliders it
    // hit (or overlapped) in all (which also keeps the compiler from optimizing the work away).
    struct Kind {
        std::string name;
        std::function<std::size_t(const std::vector<BenchmarkQuery>&)> runBatch;
        int queriesPerBatch = 0;

        double totalMicroseconds = 0.0;
        double bestBatchMicroseconds = std::numeric_limits<double>::max();
        std::size_t totalHits = 0;
    };

}  // namespace

float GetBenchmarkAreaSize(const int colliders) noexcept {
    return std::max(std::sqrt(static_cast<float>(std::max(colliders, 0))) * kGroundPerCollider, kMinAreaSize);
}

void GenerateBenchmarkScene(Frost::Level& level, const Frost::PhysicsBenchmarkOptions& options) {
    Random random(options.seed);
    const float areaSize = GetBenchmarkAreaSize(options.colliders);

    for (int i = 0; i < options.colliders; ++i) {
        const bool box = i % 2 == 0;
        Frost::GameObject& object = level.CreateObject((box ? "Box " : "Capsule ") + std::to_string(i + 1));
        Frost::TransformComponent& transform = object.GetTransform();
        transform.position = RandomPosition(random, areaSize);
        transform.rotation = ToEulerDegrees(random.Rotation());

        if (box) {
            Frost::BoxColliderComponent collider;
            collider.size = glm::vec3(random.Uniform(kMinBoxSize, kMaxBoxSize), random.Uniform(kMinBoxSize, kMaxBoxSize),
                                      random.Uniform(kMinBoxSize, kMaxBoxSize));
            object.AddComponent<Frost::BoxColliderComponent>(collider);
        }
        else {
            Frost::CapsuleColliderComponent collider;
            collider.radius = random.Uniform(kMinCapsuleRadius, kMaxCapsuleRadius);
            collider.height = random.Uniform(collider.radius * 2.0f, kMaxCapsuleHeight);
            object.AddComponent<Frost::CapsuleColliderComponent>(collider);
        }
    }
}

std::vector<BenchmarkQuery> GenerateBenchmarkQueries(const Frost::PhysicsBenchmarkOptions& options) {
    // A stream of its own, so the queries do not depend on how many numbers the scene used.
    Random random(options.seed ^ 0x9E3779B9u);
    const float areaSize = GetBenchmarkAreaSize(options.colliders);

    std::vector<BenchmarkQuery> queries;
    queries.reserve(static_cast<std::size_t>(options.queries));
    for (int i = 0; i < options.queries; ++i) {
        const glm::vec3 origin = RandomPosition(random, areaSize);
        const glm::vec3 direction = random.Direction();
        queries.push_back(BenchmarkQuery{
                .ray = Frost::Ray{.origin = origin, .direction = direction},
                .box = Frost::OrientedBox{.center = origin,
                                          .axes = glm::mat3_cast(random.Rotation()),
                                          .halfExtents = glm::vec3(kOverlapBoxHalfSize)}});
    }
    return queries;
}

std::vector<BenchmarkResult> RunPhysicsBenchmark(const Frost::PhysicsBenchmarkOptions& options, std::ostream& out) {
    out << std::fixed;
#ifndef NDEBUG
    out << kPrefix << "WARNING: this is a debug build; its timings are far slower than a release build's.\n";
#endif

    // The scene: a level that only ever lives here (it is never saved, and no level is loaded).
    const float areaSize = GetBenchmarkAreaSize(options.colliders);
    out << kPrefix << "Generating a scene of " << options.colliders << " colliders (" << (options.colliders + 1) / 2
        << " boxes, " << options.colliders / 2 << " capsules) over " << std::setprecision(0) << areaSize / Frost::kMeter
        << " m x " << areaSize / Frost::kMeter << " m, up to " << kBenchmarkAreaHeight / Frost::kMeter
        << " m high, seed " << options.seed << " (in memory only: nothing is saved)..." << std::endl;
    Clock::time_point start = Clock::now();
    Frost::Level level("Physics Benchmark");
    GenerateBenchmarkScene(level, options);
    const std::vector<BenchmarkQuery> queries = GenerateBenchmarkQueries(options);
    const Frost::Registry& registry = level.GetRegistry();
    out << kPrefix << "Generated in " << std::setprecision(1) << MicrosecondsSince(start) / 1000.0 << " ms." << std::endl;

    std::vector<Kind> kinds;
    const auto addKind = [&](std::string name, auto perQuery) {
        kinds.push_back(Kind{.name = std::move(name),
                             .runBatch = [perQuery](const std::vector<BenchmarkQuery>& batch) {
                                 std::size_t hits = 0;
                                 for (const BenchmarkQuery& query : batch) { hits += perQuery(query); }
                                 return hits;
                             },
                             .queriesPerBatch = options.queries});
    };
    const auto count = [](const auto& result) -> std::size_t {
        if constexpr (requires { result.size(); }) { return result.size(); }
        else { return result.has_value() ? 1 : 0; }
    };
    addKind("Raycast (closest)", [&](const BenchmarkQuery& q) { return count(Frost::RaycastClosest(registry, q.ray, kReach)); });
    addKind("Raycast (all)", [&](const BenchmarkQuery& q) { return count(Frost::RaycastAll(registry, q.ray, kReach)); });
    addKind("Sphere cast (closest)", [&](const BenchmarkQuery& q) {
        return count(Frost::SphereCastClosest(registry, q.ray, kCastSphereRadius, kReach));
    });
    addKind("Sphere cast (all)", [&](const BenchmarkQuery& q) {
        return count(Frost::SphereCastAll(registry, q.ray, kCastSphereRadius, kReach));
    });
    addKind("Capsule cast (closest)", [&](const BenchmarkQuery& q) {
        return count(Frost::CapsuleCastClosest(registry, MakeCastCapsule(q.ray.origin), q.ray.direction, kReach));
    });
    addKind("Capsule cast (all)", [&](const BenchmarkQuery& q) {
        return count(Frost::CapsuleCastAll(registry, MakeCastCapsule(q.ray.origin), q.ray.direction, kReach));
    });
    addKind("Sphere overlap", [&](const BenchmarkQuery& q) { return count(Frost::OverlapSphere(registry, q.ray.origin, kOverlapSphereRadius)); });
    addKind("Box overlap", [&](const BenchmarkQuery& q) { return count(Frost::OverlapBox(registry, q.box)); });

    // The game's own path: Physics Query components (the five kinds in turn, nearest hit only),
    // all updated together, once per frame, as the game does. Their objects have no colliders, so
    // they change nothing for the queries above.
    {
        constexpr Frost::PhysicsQueryType kTypes[] = {Frost::PhysicsQueryType::Raycast, Frost::PhysicsQueryType::SphereCast,
                                                      Frost::PhysicsQueryType::CapsuleCast,
                                                      Frost::PhysicsQueryType::SphereOverlap, Frost::PhysicsQueryType::BoxOverlap};
        for (std::size_t i = 0; i < queries.size(); ++i) {
            Frost::GameObject& object = level.CreateObject("Query " + std::to_string(i + 1));
            Frost::TransformComponent& transform = object.GetTransform();
            transform.position = queries[i].ray.origin;
            Frost::PhysicsQueryComponent query;
            query.type = kTypes[i % std::size(kTypes)];
            // The same queries as above: a box overlap's object is turned as the box is (the others'
            // are not, so capsules stand up), and the direction is given in the object's axes.
            if (query.type == Frost::PhysicsQueryType::BoxOverlap) {
                transform.rotation = ToEulerDegrees(glm::quat_cast(queries[i].box.axes));
            }
            query.direction = glm::inverse(transform.GetRotation()) * queries[i].ray.direction;
            query.maxDistance = kReach;
            switch (query.type) {
                case Frost::PhysicsQueryType::SphereOverlap: query.radius = kOverlapSphereRadius; break;
                case Frost::PhysicsQueryType::CapsuleCast: query.radius = kCastCapsuleRadius; break;
                default: query.radius = kCastSphereRadius; break;
            }
            query.height = kCastCapsuleHeight;
            query.boxSize = glm::vec3(kOverlapBoxHalfSize * 2.0f);
            object.AddComponent<Frost::PhysicsQueryComponent>(query);
        }
        Frost::Registry& mutableRegistry = level.GetRegistry();
        kinds.push_back(Kind{.name = "Physics Query components",
                             .runBatch =
                                     [&mutableRegistry](const std::vector<BenchmarkQuery>&) {
                                         Frost::UpdatePhysicsQueries(mutableRegistry);  // One frame.
                                         std::size_t hits = 0;
                                         mutableRegistry.Each<Frost::PhysicsQueryComponent>(
                                                 [&](Frost::EntityId, const Frost::PhysicsQueryComponent& query) {
                                                     hits += query.touching.size();
                                                 });
                                         return hits;
                                     },
                             .queriesPerBatch = options.queries});
    }

    out << kPrefix << options.queries << " queries of each kind per batch, " << options.batches
        << " timed batches after 1 warm-up. Casts reach " << std::setprecision(0) << kReach / Frost::kMeter
        << " m; sphere casts " << kCastSphereRadius << " cm, capsule casts " << kCastCapsuleRadius << " x "
        << kCastCapsuleHeight << " cm; overlaps: sphere " << kOverlapSphereRadius << " cm, box "
        << kOverlapBoxHalfSize * 2.0f << " cm." << std::endl;

    // Batch by batch, each kind in turn (so a slow patch of the machine's time spreads over all of
    // them rather than landing on one); batch 0 is the warm-up, which also tells how long the rest
    // will take (every query visits every collider, so big scenes take a while).
    for (int batch = 0; batch <= options.batches; ++batch) {
        out << kPrefix << (batch == 0 ? "Warm-up batch" : "Batch " + std::to_string(batch) + "/" + std::to_string(options.batches))
            << "..." << std::flush;
        const Clock::time_point batchStart = Clock::now();
        for (Kind& kind : kinds) {
            start = Clock::now();
            const std::size_t hits = kind.runBatch(queries);
            const double microseconds = MicrosecondsSince(start);
            if (batch == 0) { continue; }
            kind.totalMicroseconds += microseconds;
            kind.bestBatchMicroseconds = std::min(kind.bestBatchMicroseconds, microseconds);
            kind.totalHits += hits;
        }
        const double seconds = MicrosecondsSince(batchStart) / 1.0e6;
        out << " " << std::setprecision(1) << seconds << " s";
        if (batch == 0 && options.batches > 0) {
            out << " (the timed batches will take about " << seconds * options.batches << " s)";
        }
        out << std::endl;
    }

    // The results.
    std::vector<BenchmarkResult> results;
    out << kPrefix << '\n'
        << kPrefix << std::left << std::setw(28) << "Query" << std::right << std::setw(14) << "Average" << std::setw(14)
        << "Best batch" << std::setw(13) << "Hits/query" << '\n';
    for (const Kind& kind : kinds) {
        const double timed = static_cast<double>(kind.queriesPerBatch) * options.batches;
        BenchmarkResult result{.name = kind.name,
                               .averageMicroseconds = kind.totalMicroseconds / timed,
                               .bestBatchMicroseconds = kind.bestBatchMicroseconds / kind.queriesPerBatch,
                               .hitsPerQuery = static_cast<double>(kind.totalHits) / timed};
        out << kPrefix << std::left << std::setw(28) << result.name << std::right << std::setprecision(2)
            << std::setw(11) << result.averageMicroseconds << " us" << std::setw(11) << result.bestBatchMicroseconds
            << " us" << std::setw(13) << result.hitsPerQuery << '\n';
        results.push_back(std::move(result));
    }
    const BenchmarkResult& components = results.back();
    out << kPrefix << "(Physics Query components: " << options.queries << " updated per frame take "
        << std::setprecision(3) << components.averageMicroseconds * options.queries / 1000.0
        << " ms a frame; times are per query.)" << std::endl;
    return results;
}
