#version 450

// WebGL-aligned portable cloud billboards. This source is not an exact native
// ksClouds shader reconstruction. The recovered native evidence proves cloud
// participation during cube capture, while public/app.js supplies these
// placement and billboard equations.

layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 cloud_offset;
layout(location = 2) in vec2 uv;
layout(location = 3) in float cloud_speed;

layout(set = 0, binding = 0, std140) uniform CloudConstants {
    mat4 view_projection;
    vec4 camera_position_time;
    vec4 light_direction_fog;
    vec4 light_color_cover;
    vec4 ambient_color_cutoff;
    vec4 cloud_color;
} cloud;

layout(location = 0) out vec2 texture_coordinates;

void main() {
    float angle = cloud_speed * cloud.camera_position_time.w;
    float cosine = cos(angle);
    float sine = sin(angle);
    vec3 offset = vec3(cloud_offset.x * cosine - cloud_offset.z * sine,
                       cloud_offset.y,
                       cloud_offset.x * sine + cloud_offset.z * cosine);

    // The native billboard uses the local normal (0, -1, 0). The fallback
    // keeps the portable pass finite when a bounded layout reaches the axis.
    float offset_length_squared = dot(offset, offset);
    vec3 heading = offset_length_squared < 1.0e-8
                       ? vec3(0.0, 0.0, -1.0)
                       : offset * inversesqrt(offset_length_squared);
    vec3 right = cross(heading, vec3(0.0, -1.0, 0.0));
    if (dot(right, right) < 1.0e-8)
        right = cross(heading, vec3(0.0, 0.0, 1.0));
    right = normalize(right);
    vec3 up = normalize(cross(heading, right));

    vec3 position = cloud.camera_position_time.xyz + offset +
                    right * corner.x + up * corner.y;
    texture_coordinates = uv;
    gl_Position = cloud.view_projection * vec4(position, 1.0);
}
