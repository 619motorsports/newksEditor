#version 450

// WebGL-aligned portable cloud shading. This is not an exact native shader
// reconstruction. The formula mirrors public/app.js:2192-2197. Alpha is
// intended for ordinary source-alpha blending, with no shader-side discard.

layout(set = 0, binding = 1) uniform sampler2D cloud_texture;

layout(set = 0, binding = 0, std140) uniform CloudConstants {
    mat4 view_projection;
    vec4 camera_position_time;
    vec4 light_direction_fog;
    vec4 light_color_cover;
    vec4 ambient_color_cutoff;
    vec4 cloud_color;
} cloud;

layout(location = 0) in vec2 texture_coordinates;
layout(location = 0) out vec4 cloud_color;

void main() {
    float fog = clamp(900.0 / max(cloud.light_direction_fog.w, 1.0e-7),
                      0.0, 1.0);
    vec4 texture_sample = texture(cloud_texture, texture_coordinates);
    float value = dot(vec3(0.0, -1.0, 0.0),
                      cloud.light_direction_fog.xyz) *
                  (1.0 - texture_sample.r) *
                  cloud.cloud_color.x;
    vec3 color = 0.5 * (cloud.light_color_cover.xyz +
                        cloud.ambient_color_cutoff.xyz) *
                 (1.0 - cloud.ambient_color_cutoff.w);
    color = mix(vec3(value), vec3(cloud.ambient_color_cutoff.x * 0.75), fog) *
            cloud.ambient_color_cutoff.w + color;
    float alpha = clamp(cloud.light_color_cover.w * texture_sample.a,
                        0.0, 1.0);
    cloud_color = vec4(color, alpha);
}
