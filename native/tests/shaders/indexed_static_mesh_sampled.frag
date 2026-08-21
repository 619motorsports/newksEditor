#version 450

layout(location = 0) in vec2 fragmentTexcoord;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform texture2D diffuseTexture;
layout(set = 0, binding = 1) uniform sampler diffuseSampler;

void main() {
    color = texture(sampler2D(diffuseTexture, diffuseSampler), fragmentTexcoord);
}
