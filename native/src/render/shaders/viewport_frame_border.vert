#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform DrawMatricesBlock {
    mat4 world;
    mat4 viewProjection;
} drawMatrices;

layout(location = 0) out vec4 vertexColor;

void main() {
    gl_Position = drawMatrices.viewProjection * drawMatrices.world *
                  vec4(inPosition, 1.0);
    vertexColor = inColor;
}
