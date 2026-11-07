#version 450

// Skinned meshes: basic.vert with linear blend skinning in front. The mesh's vertices are in its
// bind pose; each is moved by up to four of the draw's joint matrices (its palette, a run of the
// frame's joint buffer), blended by its weights, before the usual model transform. Drawn with
// basic.frag, so the outputs match basic.vert's exactly.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;  // xyz tangent, w bitangent handedness (+1/-1); zero if unknown.
// The skin stream (binding 1). Mirrored by Frost::SkinnedVertex.
layout(location = 5) in uvec4 inJoints;  // Into the draw's palette.
layout(location = 6) in vec4 inWeights;  // Sum to 1, or all 0 for a vertex the palette does not move.

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragNormal;   // World space; normalized again per fragment.
layout(location = 3) out vec4 fragTangent;  // World space xyz + handedness.
layout(location = 4) out vec3 fragWorldPosition;

layout(set = 0, binding = 0) uniform FrameUniforms {
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 ambientColor;
    uvec4 lightCount;
} frame;

// The frame's joint matrices (set 0, binding 2), written by the renderer from the palettes of the
// skinned draws submitted this frame. Mirrored by Frost::GpuJointMatrix.
layout(std430, set = 0, binding = 2) readonly buffer JointBuffer {
    mat4 joints[];
};

// Per-draw constants, pushed by Mesh::Draw. Mirrored by Frost::MeshPushConstants: only the normal
// matrix's 3x3 is a matrix; normalMatrix[3].x is the draw's first joint in `joints` (see
// Frost::SetFirstJoint).
layout(push_constant) uniform MeshConstants {
    mat4 model;
    mat4 normalMatrix;
} mesh;

// The matrix that takes normals along with `m`: its cofactor matrix, which is the inverse-transpose
// scaled by the determinant (so it is exact under non-uniform scale, and cheap), with the sign of
// the determinant divided back out so a mirroring matrix does not turn normals inside out. The
// result is normalized later.
mat3 NormalMatrixOf(const mat3 m) {
    const mat3 cofactor = mat3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    return dot(m[0], cofactor[0]) < 0.0 ? -cofactor : cofactor;
}

void main() {
    const uint firstJoint = uint(mesh.normalMatrix[3].x + 0.5);

    mat4 skin = mat4(1.0);
    const float totalWeight = inWeights.x + inWeights.y + inWeights.z + inWeights.w;
    if (totalWeight > 0.0) {
        skin = inWeights.x * joints[firstJoint + inJoints.x] + inWeights.y * joints[firstJoint + inJoints.y] +
               inWeights.z * joints[firstJoint + inJoints.z] + inWeights.w * joints[firstJoint + inJoints.w];
    }

    const vec4 worldPosition = mesh.model * (skin * vec4(inPosition, 1.0));

    gl_Position = frame.projection * frame.view * worldPosition;

    fragColor = inColor;
    fragUV = inUV;
    fragWorldPosition = worldPosition.xyz;

    const mat3 skinLinear = mat3(skin);
    fragNormal = normalize(mat3(mesh.normalMatrix) * (NormalMatrixOf(skinLinear) * inNormal));
    fragTangent = vec4(mat3(mesh.model) * (skinLinear * inTangent.xyz), inTangent.w);
}
