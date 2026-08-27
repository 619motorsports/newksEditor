#version 450

layout(push_constant) uniform DrawMatrices {
    mat4 world;
    mat4 viewProjection;
} draw;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 vertexColor;

void main() {
    gl_Position = draw.viewProjection * draw.world * vec4(inPosition, 1.0);
    vertexColor = inColor;
}
