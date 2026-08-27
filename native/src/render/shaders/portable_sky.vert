#version 450

// WebGL-aligned portable sky fullscreen triangle. Vulkan's positive-height
// viewport maps NDC Y in the opposite screen direction, so invert clip-space
// Y while retaining the WebGL NDC value for the fragment formula.
layout(location = 0) out vec2 ndc;

void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2,
                         gl_VertexIndex & 2);
    vec2 clip = position * 2.0 - 1.0;
    ndc = clip;
    gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);
}
