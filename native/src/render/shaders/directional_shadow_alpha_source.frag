#version 450

layout(location = 0) in vec2 fragmentTexcoord;

layout(set = 0, binding = 0) uniform texture2D txDiffuse;
layout(set = 0, binding = 3) uniform sampler samLinearShadow;
layout(set = 0, binding = 4, std140) uniform cbMaterial {
    vec4 lighting;
    vec4 emissiveAndAlphaRef;
} material;

void main() {
    float alpha = texture(
        sampler2D(txDiffuse, samLinearShadow), fragmentTexcoord).a;
    if (alpha < material.emissiveAndAlphaRef.w)
        discard;
}
