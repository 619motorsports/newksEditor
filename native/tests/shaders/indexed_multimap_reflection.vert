#version 450

layout(location = 0) in vec3 position;

layout(push_constant) uniform DrawMatrices {
  mat4 world;
  mat4 viewProjection;
} drawMatrices;

void main() {
  gl_Position = drawMatrices.viewProjection * drawMatrices.world *
                vec4(position, 1.0);
}
