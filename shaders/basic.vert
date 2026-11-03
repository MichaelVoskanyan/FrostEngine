#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;  // xyz tangent, w bitangent handedness (+1/-1); zero if unknown.

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragNormal;   // World space; normalized again per fragment.
layout(location = 3) out vec4 fragTangent;  // World space xyz + handedness.
layout(location = 4) out vec3 fragWorldPosition;

// Per-frame constants (set 0), written once per frame by the renderer. Mirrored by
// Frost::FrameUniforms; identity view and a placeholder projection until a camera exists.
layout(set = 0, binding = 0) uniform FrameUniforms {
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 ambientColor;
} frame;

// Per-draw constants, pushed by Mesh::Draw. Mirrored by Frost::MeshPushConstants.
layout(push_constant) uniform MeshConstants {
    mat4 model;
    mat4 normalMatrix;  // inverse-transpose of model's linear part; only its mat3 is used.
} mesh;

void main() {
    const vec4 worldPosition = mesh.model * vec4(inPosition, 1.0);

    gl_Position = frame.projection * frame.view * worldPosition;

    fragColor = inColor;
    fragUV = inUV;
    fragWorldPosition = worldPosition.xyz;
    // Normals do not transform like positions under non-uniform scale; the normal matrix keeps
    // them perpendicular to the surface in world space. Tangents lie in the surface and transform
    // as plain directions.
    fragNormal = normalize(mat3(mesh.normalMatrix) * inNormal);
    fragTangent = vec4(mat3(mesh.model) * inTangent.xyz, inTangent.w);
}
