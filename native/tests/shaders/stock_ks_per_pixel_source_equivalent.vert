#version 450

// Evidence-backed source equivalent for the shared stock ksPerPixel vertex
// stage. The register and instruction evidence is recorded in
// docs/evidence/stock-ks-per-pixel-native-abi.md. The installed DXBC hashes
// are documented there; the native package is not redistributed here.
//
// The host transposes matrices before upload. Therefore each vec4 in these
// arrays is one row used by the native dp4 instructions. Explicit dot calls
// preserve that recovered operation order and avoid depending on GLSL's
// matrix-major convention.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexcoord;

layout(std140, set = 0, binding = 0) uniform CbCamera {
    vec4 cbCamera[14]; // 14 * 16 = recovered b0 range of 224 bytes
};
layout(std140, set = 0, binding = 1) uniform CbPerObject {
    vec4 cbPerObject[4];
};
layout(std140, set = 0, binding = 2) uniform CbLighting {
    vec4 cbLighting[10]; // 10 * 16 = recovered b2 range of 160 bytes
};
layout(std140, set = 0, binding = 3) uniform CbShadowMaps {
    vec4 cbShadowMaps[13]; // 13 * 16 = recovered b3 range of 208 bytes
};

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outCameraToSurface;
layout(location = 2) out vec2 outTexcoord;
layout(location = 3) out float outFog;
layout(location = 4) out vec4 outShadow0;
layout(location = 5) out vec4 outShadow1;
layout(location = 6) out vec4 outShadow2;

void main() {
    vec4 position = vec4(inPosition, 1.0);
    vec4 worldPosition = vec4(
        dot(position, cbPerObject[0]),
        dot(position, cbPerObject[1]),
        dot(position, cbPerObject[2]),
        dot(position, cbPerObject[3]));

    vec4 viewPosition = vec4(
        dot(worldPosition, cbCamera[0]),
        dot(worldPosition, cbCamera[1]),
        dot(worldPosition, cbCamera[2]),
        dot(worldPosition, cbCamera[3]));
    gl_Position = vec4(
        dot(viewPosition, cbCamera[4]),
        dot(viewPosition, cbCamera[5]),
        dot(viewPosition, cbCamera[6]),
        dot(viewPosition, cbCamera[7]));

    outNormal = vec3(
        dot(inNormal, cbPerObject[0].xyz),
        dot(inNormal, cbPerObject[1].xyz),
        dot(inNormal, cbPerObject[2].xyz));
    outCameraToSurface = worldPosition.xyz - cbCamera[12].xyz;
    outTexcoord = inTexcoord;

    float q = (-viewPosition.z / cbLighting[5].w) * 5.77078009;
    // SM4 DXBC `exp` is base-2 exponential. `exp2` preserves that native
    // instruction meaning; using GLSL `exp` here would be a semantic change.
    float expPositive = exp2(q);
    float expNegative = exp2(-q);
    float fogShape = clamp(
        (expPositive - expNegative) / (expNegative + expPositive),
        0.0,
        1.0);
    outFog = fogShape * cbLighting[6].x;

    outShadow0 = vec4(
        dot(worldPosition, cbShadowMaps[0]),
        dot(worldPosition, cbShadowMaps[1]),
        dot(worldPosition, cbShadowMaps[2]),
        dot(worldPosition, cbShadowMaps[3]));
    outShadow1 = vec4(
        dot(worldPosition, cbShadowMaps[4]),
        dot(worldPosition, cbShadowMaps[5]),
        dot(worldPosition, cbShadowMaps[6]),
        dot(worldPosition, cbShadowMaps[7]));
    outShadow2 = vec4(
        dot(worldPosition, cbShadowMaps[8]),
        dot(worldPosition, cbShadowMaps[9]),
        dot(worldPosition, cbShadowMaps[10]),
        dot(worldPosition, cbShadowMaps[11]));
}
