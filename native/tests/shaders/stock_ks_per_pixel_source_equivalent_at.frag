#version 450

// Evidence-backed source equivalent for the AT stock ksPerPixel pixel
// stage. See docs/evidence/stock-ks-per-pixel-native-abi.md and the recorded
// AT pixel SHA-256 there. Alpha-to-coverage is pipeline state selected by the
// package flag; this shader does not discard fragments.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inCameraToSurface;
layout(location = 2) in vec2 inTexcoord;
layout(location = 3) in float inFog;
layout(location = 4) in vec4 inShadow0;
layout(location = 5) in vec4 inShadow1;
layout(location = 6) in vec4 inShadow2;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 2) uniform CbLighting {
    vec4 cbLighting[10]; // 10 * 16 = recovered b2 range of 160 bytes
};
layout(std140, set = 0, binding = 3) uniform CbShadowMaps {
    vec4 cbShadowMaps[13];
};
layout(std140, set = 0, binding = 4) uniform CbMaterial {
    vec4 cbMaterial[2];
};

layout(set = 1, binding = 0) uniform texture2D txDiffuse;
layout(set = 1, binding = 6) uniform texture2D txShadow0;
layout(set = 1, binding = 7) uniform texture2D txShadow1;
layout(set = 1, binding = 8) uniform texture2D txShadow2;
layout(set = 2, binding = 0) uniform sampler samLinear;
layout(set = 2, binding = 1) uniform samplerShadow samShadow;

float compareShadow(int cascade, vec3 coordinate) {
    // DXBC sample_c_lz requests an explicit level-zero lookup. Do not use
    // implicit fragment derivatives here: textureLod preserves that choice.
    if (cascade == 0) {
        return textureLod(sampler2DShadow(txShadow0, samShadow), coordinate, 0.0);
    }
    if (cascade == 1) {
        return textureLod(sampler2DShadow(txShadow1, samShadow), coordinate, 0.0);
    }
    return textureLod(sampler2DShadow(txShadow2, samShadow), coordinate, 0.0);
}

float sampleNineTap(int cascade, vec2 center, float depth,
                    float reciprocalWidth) {
    // The DXBC contains nine explicit sample_c_lz instructions. The
    // coordinates below are their recovered 3x3 order. A GLSL texture call
    // delegates comparison precision and edge behavior to the bound Vulkan
    // sampler, which must use the recovered LESS comparison state.
    float result = 0.0;
    result += compareShadow(cascade,
                            vec3(center + vec2(-reciprocalWidth, -reciprocalWidth), depth));
    result += compareShadow(cascade,
                            vec3(center + vec2(0.0, -reciprocalWidth), depth));
    result += compareShadow(cascade,
                            vec3(center + vec2(reciprocalWidth, -reciprocalWidth), depth));
    result += compareShadow(cascade,
                            vec3(center + vec2(-reciprocalWidth, 0.0), depth));
    result += compareShadow(cascade, vec3(center, depth));
    result += compareShadow(cascade,
                            vec3(center + vec2(reciprocalWidth, 0.0), depth));
    result += compareShadow(cascade,
                            vec3(center + vec2(-reciprocalWidth, reciprocalWidth), depth));
    result += compareShadow(cascade,
                            vec3(center + vec2(0.0, reciprocalWidth), depth));
    result += compareShadow(cascade,
                            vec3(center + vec2(reciprocalWidth, reciprocalWidth), depth));
    return result * 0.111111112;
}

float sampleCascade(vec4 cascade, int cascadeIndex, float bias,
                    float reciprocalWidth) {
    // The recovered bounds tests are strict and check only x and y.
    if (cascade.x <= -1.0 || cascade.x >= 1.0 ||
        cascade.y <= -1.0 || cascade.y >= 1.0) {
        return 1.0;
    }

    vec2 center = vec2(cascade.x * 0.5 + 0.5,
                       cascade.y * -0.5 + 0.5);
    float adjustedDepth = cascade.z - bias * (2048.0 * reciprocalWidth);
    return sampleNineTap(cascadeIndex, center, adjustedDepth, reciprocalWidth);
}

float shadowFactor() {
    float reciprocalWidth = cbShadowMaps[12].w;
    float shadow = 1.0;

    if (inShadow0.x > -1.0 && inShadow0.x < 1.0 &&
        inShadow0.y > -1.0 && inShadow0.y < 1.0) {
        shadow = sampleCascade(inShadow0, 0,
                               cbShadowMaps[12].x, reciprocalWidth);
    } else if (inShadow1.x > -1.0 && inShadow1.x < 1.0 &&
               inShadow1.y > -1.0 && inShadow1.y < 1.0) {
        shadow = sampleCascade(inShadow1, 1,
                               cbShadowMaps[12].y, reciprocalWidth);
    } else if (inShadow2.x > -1.0 && inShadow2.x < 1.0 &&
               inShadow2.y > -1.0 && inShadow2.y < 1.0) {
        shadow = sampleCascade(inShadow2, 2,
                               cbShadowMaps[12].z, reciprocalWidth);
        float cascadeTwoFade = max(-(inShadow2.y + 0.5), 0.0) * 2.0;
        shadow = min(shadow + cascadeTwoFade, 1.0);
    }
    return shadow;
}

void main() {
    vec4 sampledDiffuse = texture(
        sampler2D(txDiffuse, samLinear), inTexcoord);
    float shadow = shadowFactor();

    // The AT package normalizes the interpolated normal before lighting.
    vec3 normal = normalize(inNormal);
    vec3 lightDirection = -cbLighting[0].xyz;
    float direct = clamp(dot(normal, lightDirection), 0.0, 1.0);
    float hemisphere = clamp(0.25 * normal.y + 0.75, 0.0, 1.0);

    vec3 baseLight = cbLighting[1].xyz * cbMaterial[0].x * hemisphere +
                     cbMaterial[1].xyz +
                     cbLighting[2].xyz * cbMaterial[0].y * direct * shadow;

    vec3 halfVector = normalize(normalize(-inCameraToSurface) + lightDirection);
    float specularPower = max(cbMaterial[0].w, 1.0);
    float specularBase = clamp(dot(normal, halfVector), 0.0, 1.0);
    // SM4 `log`/`exp` are base-2 instructions. Keep the recovered operation
    // pair explicit instead of relying on the implementation of `pow`.
    float specularFactor = exp2(log2(specularBase) * specularPower) *
                           cbMaterial[0].z * shadow;
    vec3 specular = cbLighting[2].xyz * specularFactor;

    vec3 lit = sampledDiffuse.rgb * baseLight + specular;
    float fog = clamp(inFog, 0.0, 1.0);
    vec3 rgb = mix(lit, cbLighting[6].yzw, fog);
    outColor = vec4(rgb, sampledDiffuse.a);
}
