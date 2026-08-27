#version 450

layout(location = 0) in vec2 fragmentTexcoord;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform texture2D diffuseTexture;
layout(set = 0, binding = 1) uniform sampler diffuseSampler;
layout(std140, set = 0, binding = 2) uniform KsPerPixelMaterial {
    vec4 lighting;
    vec4 fresnel;
    vec4 emissive;
} material;

void main() {
    vec4 diffuse = texture(sampler2D(diffuseTexture, diffuseSampler), fragmentTexcoord);
    color = diffuse * material.lighting.y + vec4(material.emissive.rgb, 0.0);
}
