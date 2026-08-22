#version 450

layout(set = 0, binding = 0) uniform texture2D diffuseTexture;
layout(set = 0, binding = 1) uniform sampler diffuseSampler;
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

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec2 sampleUv = clamp(uv, vec2(0.0), vec2(1.0));
    float d0 = texture(sampler2D(txShadow0, shadowSampler), sampleUv).r;
    float d1 = texture(sampler2D(txShadow1, shadowSampler), sampleUv).r;
    float d2 = texture(sampler2D(txShadow2, shadowSampler), sampleUv).r;
    float contractUse = shadowReceiver.splitDistances.x * 0.0;
    color = vec4(d0 + contractUse, d1, d2, 1.0);
}
