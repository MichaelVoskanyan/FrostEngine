#pragma once

#include <Frost/Physics/Collision.hpp>
#include <Frost/Project/GameLaunchOptions.hpp>
#include <Frost/Scene/Ray.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>

#include <iosfwd>
#include <string>
#include <vector>

namespace Frost {
    class Level;
}

// The physics query benchmark (FrostGame --benchmark; see Frost::PhysicsBenchmarkOptions): a scene
// of random colliders is generated in memory (never saved), batches of each kind of query are
// timed on it, and the average time of each is printed. Runs without the engine (no window, no
// renderer), so nothing but the queries is measured.

// Where the benchmark's scene is: colliders are spread over a square of ground this wide (growing
// with their number, so they stay about as dense), between the ground and this height.
[[nodiscard]]
float GetBenchmarkAreaSize(int colliders) noexcept;

inline constexpr float kBenchmarkAreaHeight = 10.0f * Frost::kMeter;

// Fills `level` (an empty one) with the scene: `options.colliders` objects, boxes and capsules
// alternately, of random sizes, positions and orientations, each with just a collider (static: no
// rigid body). The same seed gives the same scene.
void GenerateBenchmarkScene(Frost::Level& level, const Frost::PhysicsBenchmarkOptions& options);

// The queries of one batch: where each starts (anywhere in the scene's volume) and which way it
// goes (any direction). Shape casts and overlaps start at the same places.
struct BenchmarkQuery {
    Frost::Ray ray;
    Frost::OrientedBox box;  // For box overlaps: centered at the ray's origin, turned at random.
};

[[nodiscard]]
std::vector<BenchmarkQuery> GenerateBenchmarkQueries(const Frost::PhysicsBenchmarkOptions& options);

// One line of the results.
struct BenchmarkResult {
    std::string name;
    double averageMicroseconds = 0.0;    // Per query, over every timed batch.
    double bestBatchMicroseconds = 0.0;  // Per query, in the fastest batch.
    double hitsPerQuery = 0.0;           // Colliders hit (or overlapped) per query, on average.
};

// Runs the benchmark, printing what it does and its results to `out`. Returns the results (one
// per kind of query, in the order printed).
std::vector<BenchmarkResult> RunPhysicsBenchmark(const Frost::PhysicsBenchmarkOptions& options, std::ostream& out);
