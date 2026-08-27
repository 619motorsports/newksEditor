#version 450

// WebGL-aligned portable sky. This intentionally is not recovered native
// SkyBox parity; the equations mirror public/app.js:2188-2191.
layout(location = 0) in vec2 ndc;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0, std140) uniform SkyConstants {
    vec4 camera_forward_tan_half_fov;
    vec4 camera_right_aspect;
    vec4 camera_up;
    vec4 horizon_color;
    vec4 sky_color;
    vec4 sun_color;
    vec4 sun_direction;
} sky;

void main() {
    vec3 ray = normalize(
        sky.camera_forward_tan_half_fov.xyz +
        sky.camera_right_aspect.xyz * ndc.x *
            sky.camera_forward_tan_half_fov.w * sky.camera_right_aspect.w +
        sky.camera_up.xyz * ndc.y * sky.camera_forward_tan_half_fov.w);
    float height = smoothstep(-0.08, 0.42, ray.y);
    vec3 c = mix(sky.horizon_color.xyz, sky.sky_color.xyz, height);
    float sun = smoothstep(cos(0.02618), cos(0.00524),
                           dot(ray, normalize(sky.sun_direction.xyz)));
    c += sky.sun_color.xyz * sun;
    color = vec4(max(c, vec3(0.0)), 1.0);
}
