#version 450

// Source-evidenced curve approximation. The selected Yebis default also uses
// glare and dither inputs, which this first native pass does not implement.

layout(set = 0, binding = 0) uniform sampler2D hdr_source;

layout(push_constant) uniform ToneMapParameters {
    float exposure;
    float gamma_value;
    float saturation;
    float curve_scale;
    float curve_shoulder;
    uint srgb_destination;
} parameters;

layout(location = 0) in vec2 texture_coordinates;
layout(location = 0) out vec4 display_color;

vec3 decode_srgb(vec3 encoded) {
    bvec3 low = lessThanEqual(encoded, vec3(0.04045));
    vec3 linear_low = encoded / 12.92;
    vec3 linear_high = pow((encoded + 0.055) / 1.055, vec3(2.4));
    return mix(linear_high, linear_low, low);
}

void main() {
    vec4 source = texture(hdr_source, texture_coordinates);
    vec3 rgb = max(source.rgb, vec3(0.0));
    float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3 value = max(vec3(1.0 / 16384.0),
                     mix(vec3(luminance), rgb, parameters.saturation) *
                         parameters.exposure);
    vec3 decay = exp(-value * parameters.curve_scale);
    vec3 shoulder = 1.0 - decay * parameters.curve_shoulder;
    vec3 curve = clamp((1.0 - decay) * shoulder * shoulder, 0.0, 1.0);
    vec3 encoded = pow(min(vec3(1.0), curve + vec3(1.0 / 4194304.0)),
                       vec3(1.0 / parameters.gamma_value));
    vec3 output_rgb = parameters.srgb_destination != 0U
                          ? decode_srgb(encoded)
                          : encoded;
    display_color = vec4(output_rgb, source.a);
}
