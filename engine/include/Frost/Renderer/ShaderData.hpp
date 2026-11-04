#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>

namespace Frost {

    // CPU-side mirrors of the data blocks the shaders declare. Layouts must match shaders/basic.*
    // exactly. Every member is a mat4, a (u)vec4 or a float placed after the vec4s in groups of four,
    // so the std140 (uniform buffer) and std430 (push constant) layouts coincide with glm's and no
    // explicit padding is needed; the static_asserts below pin that down.

    // Per-frame constants: descriptor set 0, binding 0, a uniform buffer rewritten every frame
    // (see Renderer::UpdateFrameUniforms). View/projection come from the renderer's camera (or a
    // placeholder when none is set); the lights themselves are in the light buffer (GpuLight).
    struct FrameUniforms {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec4 cameraPosition{0.0f, 0.0f, 0.0f, 1.0f};  // World space; the view matrix's inverse translation.
        glm::vec4 ambientColor{0.0f};                        // rgb; stand-in for image-based lighting.
        glm::uvec4 lightCount{0u};                           // x = number of valid GpuLights this frame.
    };

    // GpuLight::directionType.w values.
    enum class GpuLightType : std::uint32_t { Directional = 0, Point = 1, Spot = 2 };

    // One light in the per-frame light buffer: descriptor set 0, binding 1, a read-only storage
    // buffer (std430) of up to Renderer::kMaxLights of these; FrameUniforms::lightCount says how
    // many are valid. Written by the renderer from the lights submitted for the frame.
    struct GpuLight {
        glm::vec4 positionRange{0.0f};   // xyz world position (point/spot), w range (point/spot).
        glm::vec4 directionType{0.0f};   // xyz direction the light travels (unit), w GpuLightType as a float.
        glm::vec4 color{0.0f};           // rgb linear color * intensity; w unused.
        glm::vec4 spotCone{0.0f};        // x cos(outer half-angle), y 1 / (cos(inner) - cos(outer)); spot only.
    };

    // Per-draw constants, pushed by Mesh::Draw from the mesh's transform: the model (object -> world)
    // matrix, and the matrix that takes normals along with it (inverse-transpose of its linear part,
    // stored as a mat4 so the block has no mat3 padding to get wrong).
    struct MeshPushConstants {
        glm::mat4 model{1.0f};
        glm::mat4 normalMatrix{1.0f};
    };

    // Per-material constants: descriptor set 1, binding 0 (the material's textures follow at
    // bindings 1-5). Written once when the Material is created; see MaterialParameters for the
    // meaning and defaults of each value.
    struct MaterialUniforms {
        glm::vec4 baseColorFactor{1.0f};
        glm::vec4 emissiveFactor{0.0f};  // rgb; w unused.
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
    };

    static_assert(sizeof(FrameUniforms) == 2 * sizeof(glm::mat4) + 3 * sizeof(glm::vec4));
    static_assert(offsetof(FrameUniforms, cameraPosition) == 2 * sizeof(glm::mat4));
    static_assert(offsetof(FrameUniforms, lightCount) == 2 * sizeof(glm::mat4) + 2 * sizeof(glm::vec4));
    static_assert(sizeof(GpuLight) == 4 * sizeof(glm::vec4));  // std430 array stride.
    // Vulkan guarantees at least 128 bytes of push constants; MeshPushConstants uses all of them.
    static_assert(sizeof(MeshPushConstants) == 128);
    static_assert(offsetof(MeshPushConstants, normalMatrix) == sizeof(glm::mat4));
    static_assert(sizeof(MaterialUniforms) == 3 * sizeof(glm::vec4));
    static_assert(offsetof(MaterialUniforms, metallicFactor) == 2 * sizeof(glm::vec4));

}  // namespace Frost
