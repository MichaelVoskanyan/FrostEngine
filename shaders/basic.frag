#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;
layout(location = 4) in vec3 fragWorldPosition;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform FrameUniforms {
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 ambientColor;  // rgb; stand-in for image-based lighting.
    uvec4 lightCount;   // x = number of valid entries in `lights`.
} frame;

// The frame's lights (set 0, binding 1), written by the renderer from whatever the application
// submitted this frame. Mirrored by Frost::GpuLight.
const uint kLightDirectional = 0u;
const uint kLightPoint = 1u;
const uint kLightSpot = 2u;

struct Light {
    vec4 positionRange;  // xyz world position (point/spot), w range.
    vec4 directionType;  // xyz direction the light travels (unit), w type (kLight*).
    vec4 color;          // rgb = linear color * intensity.
    vec4 spotCone;       // x cos(outer half-angle), y 1 / (cos(inner) - cos(outer)).
};

layout(std430, set = 0, binding = 1) readonly buffer LightBuffer {
    Light lights[];
};

// Set 1 is the material (see VulkanDescriptorAllocator / Frost::Material): its parameters at
// binding 0, its textures after that in MaterialTextures order. Mesh::Draw binds each sub-mesh's
// material, or the renderer's default, before its draw call. Unset texture slots hold neutral
// defaults (white, or a flat normal), so every slot is always sampled.
layout(set = 1, binding = 0) uniform MaterialUniforms {
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
} material;

layout(set = 1, binding = 1) uniform sampler2D baseColorTexture;          // sRGB.
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessTexture;  // G = roughness, B = metallic.
layout(set = 1, binding = 3) uniform sampler2D normalTexture;             // Tangent space, RGB -> [-1, 1].
layout(set = 1, binding = 4) uniform sampler2D occlusionTexture;          // R = ambient occlusion.
layout(set = 1, binding = 5) uniform sampler2D emissiveTexture;           // sRGB.

const float kPi = 3.14159265359;
// Perfectly smooth surfaces make the GGX lobe a delta function (0/0 below); clamp like most
// real-time renderers do.
const float kMinRoughness = 0.045;
// Reflectance at normal incidence of dielectrics (4%, glTF's convention).
const vec3 kDielectricF0 = vec3(0.04);

// The surface normal after normal mapping: the sampled tangent-space normal expressed in the
// world-space frame built from the interpolated normal and tangent. Falls back to the geometric
// normal where the mesh has no tangent frame.
vec3 ShadingNormal() {
    const vec3 n = normalize(fragNormal);

    if (dot(fragTangent.xyz, fragTangent.xyz) < 1e-12) { return n; }

    // Re-orthogonalize after interpolation; the handedness sign picks the bitangent direction.
    const vec3 t = normalize(fragTangent.xyz - n * dot(n, fragTangent.xyz));
    const vec3 b = cross(n, t) * fragTangent.w;

    vec3 tangentNormal = texture(normalTexture, fragUV).xyz * 2.0 - 1.0;
    tangentNormal.xy *= material.normalScale;

    return normalize(mat3(t, b, n) * tangentNormal);
}

// Inverse-square falloff (physically correct for a point source), windowed so it reaches exactly
// zero at the light's range instead of trailing off forever: (1 - (d/r)^4)^2, the window used by
// Frostbite and Unreal. The 1 cm floor keeps a surface touching the light from blowing up.
float DistanceAttenuation(float distanceSquared, float range) {
    const float inverseSquare = 1.0 / max(distanceSquared, 0.0001);
    const float ratio = distanceSquared / max(range * range, 0.0001);
    const float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
    return inverseSquare * window * window;
}

// Spot cone: full strength inside the inner cone, a smooth fade to zero at the outer cone.
float SpotAttenuation(vec3 lightTravel, vec3 toLight, vec4 spotCone) {
    const float cosAngle = dot(lightTravel, -toLight);
    const float t = clamp((cosAngle - spotCone.x) * spotCone.y, 0.0, 1.0);
    return t * t;
}

// Metallic-roughness BRDF as specified by glTF (Appendix B): Lambert diffuse plus a GGX specular
// lobe with the height-correlated Smith visibility term and Schlick's Fresnel approximation.
vec3 Brdf(vec3 n, vec3 v, vec3 l, vec3 diffuseColor, vec3 f0, float roughness) {
    const vec3 h = normalize(v + l);
    const float nDotL = max(dot(n, l), 0.0);
    const float nDotV = max(dot(n, v), 1e-4);
    const float nDotH = max(dot(n, h), 0.0);
    const float vDotH = max(dot(v, h), 0.0);

    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;

    const float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    const float distribution = alpha2 / (kPi * denominator * denominator);

    const float ggxV = nDotL * sqrt(nDotV * nDotV * (1.0 - alpha2) + alpha2);
    const float ggxL = nDotV * sqrt(nDotL * nDotL * (1.0 - alpha2) + alpha2);
    const float visibility = ggxV + ggxL > 0.0 ? 0.5 / (ggxV + ggxL) : 0.0;

    const vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vDotH, 5.0);

    const vec3 diffuse = (1.0 - fresnel) * diffuseColor / kPi;
    const vec3 specular = fresnel * distribution * visibility;

    return (diffuse + specular) * nDotL;
}

void main() {
    // Material inputs. Base color = factor * texture * vertex color; the remaining textures each
    // scale their factor, in the channels glTF assigns them.
    const vec4 baseColor = material.baseColorFactor * texture(baseColorTexture, fragUV) * fragColor;
    const vec2 metallicRoughness = texture(metallicRoughnessTexture, fragUV).bg;
    const float metallic = clamp(material.metallicFactor * metallicRoughness.x, 0.0, 1.0);
    const float roughness = clamp(material.roughnessFactor * metallicRoughness.y, kMinRoughness, 1.0);
    const float occlusion = 1.0 + material.occlusionStrength * (texture(occlusionTexture, fragUV).r - 1.0);
    const vec3 emissive = material.emissiveFactor.rgb * texture(emissiveTexture, fragUV).rgb;

    // Metals have no diffuse reflection and take their specular color from the base color;
    // dielectrics reflect 4% and diffuse the rest.
    const vec3 diffuseColor = baseColor.rgb * (1.0 - metallic);
    const vec3 f0 = mix(kDielectricF0, baseColor.rgb, metallic);

    const vec3 n = ShadingNormal();
    // An orthographic projection (projection[3][3] == 1) has one view direction everywhere: the
    // third row of the view rotation; a perspective one looks from the camera position.
    const vec3 orthographicToViewer = vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2]);
    const vec3 v = normalize(frame.projection[3][3] == 1.0 ? orthographicToViewer
                                                           : frame.cameraPosition.xyz - fragWorldPosition);
    // Every light of the frame, plus a flat ambient term (occluded) standing in for environment
    // lighting until image-based lighting exists; emissive light is added unlit.
    vec3 direct = vec3(0.0);
    for (uint i = 0u; i < frame.lightCount.x; ++i) {
        const Light light = lights[i];
        const uint type = uint(light.directionType.w + 0.5);
        const vec3 travel = light.directionType.xyz;

        vec3 l;
        float attenuation = 1.0;

        if (type == kLightDirectional) {
            l = -travel;
        }
        else {
            const vec3 toLight = light.positionRange.xyz - fragWorldPosition;
            const float distanceSquared = dot(toLight, toLight);
            if (distanceSquared >= light.positionRange.w * light.positionRange.w) { continue; }  // Out of range.

            l = toLight * inversesqrt(max(distanceSquared, 1e-8));
            attenuation = DistanceAttenuation(distanceSquared, light.positionRange.w);
            if (type == kLightSpot) { attenuation *= SpotAttenuation(travel, l, light.spotCone); }
        }

        if (attenuation <= 0.0) { continue; }
        direct += Brdf(n, v, l, diffuseColor, f0, roughness) * light.color.rgb * attenuation;
    }

    const vec3 ambient = frame.ambientColor.rgb * (diffuseColor + f0) * occlusion;

    outColor = vec4(direct + ambient + emissive, baseColor.a);
}
