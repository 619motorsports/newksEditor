#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord;

layout(location = 0) out vec3 fragmentNormal;
layout(location = 1) out vec2 fragmentTexcoord;

layout(push_constant) uniform DrawMatrices {
    mat4 world;
    mat4 viewProjection;
} drawMatrices;

void main() {
    vec4 worldPosition = drawMatrices.world * vec4(position, 1.0);
    fragmentNormal = mat3(drawMatrices.world) * normal;
    fragmentTexcoord = texcoord;
    gl_Position = drawMatrices.viewProjection * worldPosition;
}
