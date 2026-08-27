#version 450

// Portable five-level bloom pass. The mode selects threshold, four-tap
// downsample, or separable Gaussian blur while keeping one descriptor ABI.
layout(set = 0, binding = 0) uniform sampler2D source_texture;

layout(std140, set = 0, binding = 1) uniform BloomParameters {
    vec4 mode_exposure_threshold_remap;
    vec4 direction_sample_count_texel_size;
    vec4 sample_offsets[4];
    vec4 sample_weights[4];
} parameters;

layout(location = 0) in vec2 texture_coordinates;
layout(location = 0) out vec4 bloom_color;

void main() {
    uint mode = uint(parameters.mode_exposure_threshold_remap.x + 0.5);
    vec3 result = vec3(0.0);
    if (mode == 0U) {
        vec3 source = texture(source_texture, texture_coordinates).rgb;
        float exposure = parameters.mode_exposure_threshold_remap.y;
        float threshold = parameters.mode_exposure_threshold_remap.z;
        float remap = parameters.mode_exposure_threshold_remap.w;
        result = min(max(source * exposure - vec3(threshold), vec3(0.0)) * remap,
                     vec3(64000.0));
    } else if (mode == 1U) {
        vec2 step_size = 0.5 / vec2(textureSize(source_texture, 0));
        result = texture(source_texture, texture_coordinates + vec2(-step_size.x, -step_size.y)).rgb;
        result += texture(source_texture, texture_coordinates + vec2(step_size.x, -step_size.y)).rgb;
        result += texture(source_texture, texture_coordinates + vec2(-step_size.x, step_size.y)).rgb;
        result += texture(source_texture, texture_coordinates + step_size).rgb;
        result *= 0.25;
    } else {
        vec2 texel = parameters.direction_sample_count_texel_size.xy /
                     vec2(textureSize(source_texture, 0));
        for (uint index = 0U; index < 15U; ++index) {
            vec4 offsets = parameters.sample_offsets[index / 4U];
            vec4 weights = parameters.sample_weights[index / 4U];
            float offset = offsets[index & 3U];
            float weight = weights[index & 3U];
            if (index < uint(parameters.direction_sample_count_texel_size.z))
                result += texture(source_texture, texture_coordinates + texel * offset).rgb * weight;
        }
    }
    bloom_color = vec4(result, 1.0);
}
