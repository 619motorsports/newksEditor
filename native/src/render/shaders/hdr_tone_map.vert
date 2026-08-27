#version 450

// Fullscreen stage for the labeled portable Yebis curve approximation.

layout(location = 0) out vec2 texture_coordinates;

void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2,
                         gl_VertexIndex & 2);
    texture_coordinates = position;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
