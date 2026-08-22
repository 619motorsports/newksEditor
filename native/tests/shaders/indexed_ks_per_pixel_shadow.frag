#version 450

layout(location = 0) in vec3 fragmentNormal;
layout(location = 1) in vec3 fragmentWorld;
layout(location = 2) in vec2 fragmentTexcoord;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform texture2D diffuseTexture;
layout(set = 0, binding = 1) uniform sampler diffuseSampler;
layout(std140, set = 0, binding = 2) uniform KsPerPixelMaterial {
    vec4 lighting;
    vec4 fresnel;
    vec4 emissive;
} material;
layout(std140, set = 0, binding = 3) uniform KsPerPixelFrame {
    vec4 sun_direction;
    vec4 sun_color;
    vec4 ambient_color;
    vec4 camera_position;
} frame;

layout(set = 0, binding = 16) uniform texture2D txShadow0;
layout(set = 0, binding = 17) uniform texture2D txShadow1;
layout(set = 0, binding = 18) uniform texture2D txShadow2;
layout(set = 0, binding = 19) uniform sampler shadowSampler;
layout(std140, set = 0, binding = 20) uniform ShadowReceiver {
    mat4 shadowMatrices[3];
    vec4 splitDistances;
    vec4 depthBiases;
    vec4 cameraPosition;
    vec4 cameraForward;
} shadowReceiver;

float sampleShadow(texture2D shadowMap, mat4 shadowMatrix, float bias) {
    vec4 projected = shadowMatrix * vec4(fragmentWorld, 1.0);
    if (projected.w <= 0.0) return 1.0;
    vec3 coordinate = projected.xyz / projected.w;
    // The retained matrix is already converted to Vulkan's [0, 1] depth
    // convention. Only x and y still need the NDC-to-texture remap.
    coordinate.xy = coordinate.xy * 0.5 + 0.5;
    if (coordinate.x <= 0.0 || coordinate.x >= 1.0 ||
        coordinate.y <= 0.0 || coordinate.y >= 1.0 ||
        coordinate.z <= 0.0 || coordinate.z >= 1.0) return 1.0;
    vec2 texel = 1.0 /
        vec2(textureSize(sampler2D(shadowMap, shadowSampler), 0));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float storedDepth = texture(
                sampler2D(shadowMap, shadowSampler),
                coordinate.xy + vec2(x, y) * texel).r;
            lit += coordinate.z - bias <= storedDepth ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

float sunShadow() {
    float depth = dot(fragmentWorld - shadowReceiver.cameraPosition.xyz,
                      shadowReceiver.cameraForward.xyz);
    if (depth <= shadowReceiver.splitDistances.x)
        return sampleShadow(txShadow0, shadowReceiver.shadowMatrices[0],
                            shadowReceiver.depthBiases.x);
    if (depth <= shadowReceiver.splitDistances.y)
        return sampleShadow(txShadow1, shadowReceiver.shadowMatrices[1],
                            shadowReceiver.depthBiases.y);
    if (depth <= shadowReceiver.splitDistances.z)
        return sampleShadow(txShadow2, shadowReceiver.shadowMatrices[2],
                            shadowReceiver.depthBiases.z);
    return 1.0;
}

void main() {
    vec3 texel = texture(sampler2D(diffuseTexture, diffuseSampler),
                         fragmentTexcoord).rgb;
    vec3 n = normalize(fragmentNormal);
    vec3 l = normalize(frame.sun_direction.xyz);
    vec3 v = normalize(frame.camera_position.xyz - fragmentWorld);
    float ndl = max(dot(n, l), 0.0);
    float mappedPower = max(1.0, material.lighting.w + 1.0);
    float shadow = sunShadow();
    vec3 spec = frame.sun_color.rgb *
                (pow(max(dot(n, normalize(l + v)), 0.0), mappedPower) *
                 material.lighting.z * shadow);
    vec3 lit = texel * (frame.ambient_color.rgb * material.lighting.x +
                        frame.sun_color.rgb * material.lighting.y * ndl * shadow +
                        material.emissive.rgb) + spec;
    color = vec4(lit, 1.0);
}
