#version 450

layout(location = 0) in vec3 position;

layout(push_constant) uniform DrawMatrices {
    mat4 world;
    mat4 viewProjection;
} drawMatrices;

void main() {
    vec4 worldPosition = drawMatrices.world * vec4(position, 1.0);
    gl_Position = drawMatrices.viewProjection * worldPosition;
}
