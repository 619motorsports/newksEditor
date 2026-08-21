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

void main() {
    vec3 texel = texture(sampler2D(diffuseTexture, diffuseSampler), fragmentTexcoord).rgb;
    vec3 n = normalize(fragmentNormal);
    vec3 l = normalize(frame.sun_direction.xyz);
    vec3 v = normalize(frame.camera_position.xyz - fragmentWorld);
    float ndl = max(dot(n, l), 0.0);
    float mappedPower = max(1.0, material.lighting.w + 1.0);
    vec3 spec = frame.sun_color.rgb *
                (pow(max(dot(n, normalize(l + v)), 0.0), mappedPower) *
                 material.lighting.z);
    vec3 lit = texel * (frame.ambient_color.rgb * material.lighting.x +
                        frame.sun_color.rgb * material.lighting.y * ndl +
                        material.emissive.rgb) + spec;
    color = vec4(lit, 1.0);
}
