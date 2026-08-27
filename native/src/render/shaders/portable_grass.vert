#version 450

// Bounded expanded PortableGrass preview.  This is a source-labelled
// cross-backend approximation, not an exact CSP or native shader claim.

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 atlas_uv;
layout(location = 3) in vec4 vertex_color;
layout(location = 4) in float vertex_wind;
layout(location = 5) in float tip_weight;

layout(set = 0, binding = 0, std140) uniform PortableGrassConstants {
    mat4 view_projection;
    vec4 camera_position_fog;
    vec4 sun_direction_fog_blend;
    vec4 sun_color;
    vec4 ambient_color_wetness;
    vec4 fog_color_time;
    vec4 wind_direction_strength;
} grass;

layout(location = 0) out vec2 texture_coordinates;
layout(location = 1) out vec4 blade_color;
layout(location = 2) out vec3 blade_normal;
layout(location = 3) out vec3 world_position;

void main() {
    vec3 displaced = position;
    vec2 wind_direction = grass.wind_direction_strength.xy;
    float wind_length = length(wind_direction);
    if (wind_length > 1.0e-8)
        wind_direction /= wind_length;
    else
        wind_direction = vec2(0.0);

    // The expanded CPU layout supplies a tile-independent zero-to-one blade
    // tip weight. This keeps the portable fallback valid for multirow atlases.
    float blade_wind = clamp(vertex_wind, 0.0, 1.0) *
                       clamp(tip_weight, 0.0, 1.0);
    float wind_strength = min(max(grass.wind_direction_strength.z, 0.0), 1.0);
    float phase = grass.fog_color_time.w * 1.7 +
                  dot(position.xz, wind_direction) * 0.09;
    float displacement = sin(phase) * 0.04 * wind_strength * blade_wind;
    displaced.xz += wind_direction * displacement;

    texture_coordinates = atlas_uv;
    blade_color = vertex_color;
    blade_normal = normal;
    world_position = displaced;
    gl_Position = grass.view_projection * vec4(displaced, 1.0);
}
