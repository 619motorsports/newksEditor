#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 2) uniform LightingProbePs { vec4 value; } lightingProbe;
layout(set = 0, binding = 3) uniform ShadowsProbePs { vec4 value; } shadowsProbe;
layout(set = 0, binding = 4) uniform MaterialProbe { vec4 value; } materialProbe;

layout(set = 1, binding = 0) uniform texture2D txDiffuse;
layout(set = 1, binding = 6) uniform texture2D txShadow0;
layout(set = 1, binding = 7) uniform texture2D txShadow1;
layout(set = 1, binding = 8) uniform texture2D txShadow2;
layout(set = 2, binding = 0) uniform sampler samLinear;
layout(set = 2, binding = 1) uniform samplerShadow samShadow;

void main() {
    vec4 diffuse = texture(sampler2D(txDiffuse, samLinear), inUv);
    float shadow0 = texture(sampler2DShadow(txShadow0, samShadow), vec3(0.5, 0.5, 0.5));
    float shadow1 = texture(sampler2DShadow(txShadow1, samShadow), vec3(0.5, 0.5, 0.5));
    float shadow2 = texture(sampler2DShadow(txShadow2, samShadow), vec3(0.5, 0.5, 0.5));
    float shadow = (shadow0 + shadow1 + shadow2) / 3.0;
    outColor = vec4(diffuse.rgb * shadow, diffuse.a) + lightingProbe.value +
               shadowsProbe.value + materialProbe.value;
}
