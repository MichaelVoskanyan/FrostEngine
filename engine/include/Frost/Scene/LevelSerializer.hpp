#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Frost {

    class AssetManager;
    class AssetRegistry;
    class Level;

    // Thrown when a level file cannot be written, read, or understood at all. Problems with the
    // content it references (a missing model or texture) are not errors; see LevelLoadResult.
    class LevelError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct LevelLoadResult {
        std::unique_ptr<Level> level;
        // Recoverable problems, e.g. "model 'x' could not be loaded". Affected objects are kept
        // (with their references intact, so saving does not lose them) but draw nothing.
        std::vector<std::string> warnings;
    };

    // Reads and writes levels as JSON (.flevel). Assets are referred to by id (see AssetId), so a
    // level keeps working when the assets it uses are renamed or moved; each reference also
    // carries the asset's path, relative to the level file's directory, as a readable fallback.
    // (Levels and assets all live in the project's content directory, so these paths stay valid
    // wherever the project is; a path outside it is stored absolute, but a project never loads
    // assets from outside its content directory - see AssetRegistry::Resolve.)
    //
    // Each object is saved as its id and name plus one entry per component it has (see
    // BuiltinComponents; each component type's entry is written and read by its ComponentSerializer
    // in LevelSerializer.cpp). A missing entry means the object does not have that component (every
    // object has a transform; a missing one is the identity).
    //
    // Lengths and speeds (positions, sizes, distances, light ranges, clip planes, cm/s) are in
    // centimeters, the world unit (Frost/Units.hpp). Levels before version 7 stored them in meters;
    // they are converted to centimeters as they are read (and saved in centimeters from then on).
    //
    // Format (version 7; versions 1 to 6 are still read: version 1 stored references as plain path
    // strings, versions before 3 had no colliders or rigid bodies, before 4 no cameras, before 5 no
    // fly controllers, before 6 no capsule colliders or character controllers and cameras had no
    // offset or rotation, and before 7 lengths were in meters):
    //   {
    //     "format": "FrostLevel", "version": 7, "name": "...",
    //     "environment": {"ambientColor": [r, g, b], "ambientIntensity": i},   (optional)
    //     "objects": [{
    //       "id": 1, "name": "...",
    //       "transform": {"position": [x, y, z], "rotation": [pitch, yaw, roll] (degrees), "scale": [x, y, z]},
    //       "mesh": {"id": "<32 hex digits>", "path": "relative/or/absolute/path.fmesh"},  (optional; {} = no model yet)
    //       "materials": [{                                         (optional, with "mesh"; sub-mesh overrides)
    //         "subMesh": 0, "subMeshName": "...",                   (name wins if the model has it)
    //         "baseColorFactor": [r, g, b, a], "metallic": m, "roughness": r, "normalScale": n,
    //         "occlusionStrength": o, "emissiveFactor": [r, g, b],
    //         "textures": {"baseColor": {"id", "path"}, "normal": null, ...} (absent = inherit, null = none)
    //       }],
    //       "light": {"type": "directional"|"point"|"spot",         (optional; missing fields = type defaults)
    //                 "enabled": b, "color": [r, g, b], "useTemperature": b, "temperature": kelvin,
    //                 "intensity": i, "range": r, "innerConeAngle": deg, "outerConeAngle": deg},
    //       "camera": {"verticalFov": deg, "nearClip": n, "farClip": f,    (optional)
    //                  "offset": [x, y, z], "rotation": [pitch, yaw, roll] (degrees)},
    //       "flyController": {"moveSpeed": s, "fastMultiplier": f, "lookSensitivity": deg},  (optional)
    //       "characterController": {"walkSpeed": s, "jumpSpeed": s, "lookSensitivity": deg},  (optional)
    //       "boxCollider": {"center": [x, y, z], "size": [x, y, z]},  (optional; object space, before scale)
    //       "capsuleCollider": {"center": [x, y, z], "radius": r, "height": h},  (optional; likewise)
    //       "rigidBody": {"simulated": b, "mass": kg}               (optional)
    //     }]
    //   }
    class LevelSerializer final {
    public:
        LevelSerializer() = delete;

        // Writes atomically (a temporary file renamed over the target), so a failed save never
        // leaves a truncated level behind. With a registry, each reference's path is updated to
        // where its asset is now. Throws LevelError.
        static void Save(const Level& level, const std::filesystem::path& path, const AssetRegistry* registry = nullptr);

        // Reads a level and loads the assets it references through `assets`. Throws LevelError if
        // the file cannot be read or is not a Frost level.
        [[nodiscard]]
        static LevelLoadResult Load(const std::filesystem::path& path, AssetManager& assets);
    };

}  // namespace Frost
