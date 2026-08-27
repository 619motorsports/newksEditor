#version 450

// Bounded expanded PortableGrass preview.  Lighting and wetness follow the
// public material direction, but this shader does not claim CSP pixel parity.

// Vulkan keeps the sampled image and sampler separate. This corresponds to
// D3D12 t0 and s0 below while retaining one explicit cross-backend ABI.
layout(set = 0, binding = 1) uniform texture2D grass_atlas_texture;
layout(set = 0, binding = 2) uniform sampler grass_sampler;

layout(set = 0, binding = 0, std140) uniform PortableGrassConstants {
    mat4 view_projection;
    vec4 camera_position_fog;
    vec4 sun_direction_fog_blend;
    vec4 sun_color;
    vec4 ambient_color_wetness;
    vec4 fog_color_time;
    vec4 wind_direction_strength;
} grass;

layout(location = 0) in vec2 texture_coordinates;
layout(location = 1) in vec4 blade_color;
layout(location = 2) in vec3 blade_normal;
layout(location = 3) in vec3 world_position;
layout(location = 0) out vec4 color;

void main() {
    vec4 atlas_sample = texture(sampler2D(grass_atlas_texture, grass_sampler),
                                 texture_coordinates);
    float alpha = atlas_sample.a * clamp(blade_color.a, 0.0, 1.0);
    if (alpha < 0.5)
        discard;

    vec3 normal = normalize(blade_normal);
    vec3 sun_direction = normalize(grass.sun_direction_fog_blend.xyz);
    float sun_diffuse = max(dot(normal, sun_direction), 0.0);
    vec3 albedo = max(atlas_sample.rgb * blade_color.rgb, vec3(0.0));
    float wetness = clamp(grass.ambient_color_wetness.w, 0.0, 1.0);
    albedo *= mix(1.0, pow(0.65, 2.2), wetness);
    vec3 lit = albedo * (grass.ambient_color_wetness.xyz * 0.35 +
                         grass.sun_color.xyz * 0.8 * sun_diffuse);

    float distance_to_camera = distance(grass.camera_position_fog.xyz,
                                        world_position);
    float fog_range = max(grass.camera_position_fog.w, 1.0);
    float fog_amount = pow(clamp(distance_to_camera / fog_range, 0.0, 1.0),
                           max(0.05, grass.sun_direction_fog_blend.w));
    lit = mix(lit, grass.fog_color_time.xyz, fog_amount);
    color = vec4(max(lit, vec3(0.0)), alpha);
}
