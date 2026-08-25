#version 450

layout(set = 0, binding = 0) uniform SelectedMeshColor {
    vec4 color;
} selected_mesh;

layout(location = 0) out vec4 output_color;

void main() {
    output_color = selected_mesh.color;
}
