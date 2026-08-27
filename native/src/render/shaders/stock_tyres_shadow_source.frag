#version 450

// Source-equivalent ksTyres directional-shadow receiver variant.
layout(location = 0) in vec3 fragmentNormal;
layout(location = 1) in vec3 fragmentTangent;
layout(location = 2) in vec3 fragmentBitangent;
layout(location = 3) in vec3 fragmentWorld;
layout(location = 4) in vec2 fragmentTexcoord;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform texture2D txDiffuse;
layout(set = 0, binding = 1) uniform sampler samDiffuse;
layout(std140, set = 0, binding = 2) uniform TyreMaterial {
    vec4 lighting;
    vec4 emissive;
} material;
layout(std140, set = 0, binding = 3) uniform TyreFrame {
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 cameraPosition;
    vec4 horizonColor;
    vec4 skyColor;
    vec4 fogColor;
    vec4 fog;
} frame;
layout(set = 0, binding = 4) uniform texture2D txNormal;
layout(set = 0, binding = 5) uniform sampler samNormal;
layout(set = 0, binding = 6) uniform texture2D txDirty;
layout(set = 0, binding = 7) uniform sampler samDirty;
layout(set = 0, binding = 8) uniform texture2D txBlur;
layout(set = 0, binding = 9) uniform sampler samBlur;
layout(set = 0, binding = 10) uniform texture2D txNormalBlur;
layout(set = 0, binding = 11) uniform sampler samNormalBlur;
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
layout(set = 0, binding = 21) uniform textureCube txCube;
layout(set = 0, binding = 22) uniform sampler samCube;
layout(std140, set = 0, binding = 23) uniform ReflectionConstants {
    vec4 tyreControls;
    vec4 reflection;
} reflection;

float sampleShadow(texture2D shadowMap, mat4 shadowMatrix, float bias, out bool inside) {
    inside = false;
    vec4 projected = shadowMatrix * vec4(fragmentWorld, 1.0);
    if (projected.w <= 0.0) return 1.0;
    vec3 coordinate = projected.xyz / projected.w;
    coordinate.xy = coordinate.xy * 0.5 + 0.5;
    if (coordinate.x <= 0.0 || coordinate.x >= 1.0 ||
        coordinate.y <= 0.0 || coordinate.y >= 1.0 ||
        coordinate.z <= 0.0 || coordinate.z >= 1.0) return 1.0;
    inside = true;
    vec2 texel = 1.0 / vec2(textureSize(sampler2D(shadowMap, shadowSampler), 0));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += coordinate.z - bias <= texture(
                sampler2D(shadowMap, shadowSampler), coordinate.xy + vec2(x, y) * texel).r
                       ? 1.0 : 0.0;
    return lit / 9.0;
}

float sunShadow() {
    bool inside = false;
    float shadow = sampleShadow(txShadow0, shadowReceiver.shadowMatrices[0],
                                shadowReceiver.depthBiases.x, inside);
    if (inside) return shadow;
    shadow = sampleShadow(txShadow1, shadowReceiver.shadowMatrices[1],
                          shadowReceiver.depthBiases.y, inside);
    if (inside) return shadow;
    shadow = sampleShadow(txShadow2, shadowReceiver.shadowMatrices[2],
                          shadowReceiver.depthBiases.z, inside);
    if (inside) return shadow;
    return 1.0;
}

void main() {
    float blurLevel = reflection.tyreControls.x;
    float dirtyLevel = reflection.tyreControls.y;
    vec4 base = mix(texture(sampler2D(txDiffuse, samDiffuse), fragmentTexcoord),
                    texture(sampler2D(txBlur, samBlur), fragmentTexcoord), blurLevel);
    vec4 dirty = texture(sampler2D(txDirty, samDirty), fragmentTexcoord);
    base.rgb = mix(base.rgb, dirty.rgb, dirty.a * dirtyLevel);
    vec3 normalTexel = texture(sampler2D(txNormal, samNormal), fragmentTexcoord).rgb * 2.0 - 1.0;
    vec3 normalBlurTexel = texture(sampler2D(txNormalBlur, samNormalBlur), fragmentTexcoord).rgb * 2.0 - 1.0;
    vec3 geometric = normalize(fragmentNormal);
    vec3 mapped = normalize(normalize(fragmentTangent) * normalTexel.x +
                            normalize(fragmentBitangent) * normalTexel.y + geometric * normalTexel.z);
    vec3 mappedBlur = normalize(normalize(fragmentTangent) * normalBlurTexel.x +
                                normalize(fragmentBitangent) * normalBlurTexel.y + geometric * normalBlurTexel.z);
    vec3 n = mix(mapped, mappedBlur, blurLevel);
    vec3 l = normalize(frame.sunDirection.xyz);
    vec3 v = normalize(frame.cameraPosition.xyz - fragmentWorld);
    float shadow = sunShadow();
    float ndl = max(dot(n, l), 0.0);
    float directSpecular = base.a * (1.0 - clamp(dirtyLevel, 0.0, 1.0)) * material.lighting.z;
    vec3 specular = frame.sunColor.rgb * (pow(max(dot(n, normalize(l + v)), 0.0),
                         max(1.0, material.lighting.w)) * directSpecular * shadow);
    vec3 lit = max(base.rgb * (frame.ambientColor.rgb * material.lighting.x +
                               frame.sunColor.rgb * material.lighting.y * ndl * shadow +
                               material.emissive.rgb) + specular, vec3(0.0));
    float facing = 1.0 - max(dot(n, v), 0.0);
    float fresnel = min(reflection.tyreControls.z + pow(facing, max(reflection.tyreControls.w, 0.0)),
                        clamp(1.0 - dirtyLevel, 0.0, 1.0) * reflection.reflection.y);
    float mipLevel = 6.0 * clamp(1.0 - material.lighting.w / 255.0, 0.0, 1.0);
    vec3 reflected = normalize(v - 2.0 * dot(v, n) * n);
    vec3 reflectedColor = textureLod(samplerCube(txCube, samCube),
                                     vec3(-reflected.x, reflected.y, reflected.z), mipLevel).rgb;
    vec3 outputColor = reflection.reflection.x > 0.5
                           ? lit + fresnel * reflectedColor
                           : mix(lit, reflectedColor, fresnel);
    color = vec4(max(outputColor, vec3(0.0)), 1.0);
}
