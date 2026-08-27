#version 450

// Source-evidenced Yebis curve approximation and fixed-phase 4x4 RGBA8 dither.
// Glare remains a separate pass.

layout(set = 0, binding = 0) uniform sampler2D hdr_source;

layout(push_constant) uniform ToneMapParameters {
    float exposure;
    float gamma_value;
    float saturation;
    float curve_scale;
    float curve_shoulder;
    uint srgb_destination;
    float dither_scale;
} parameters;

layout(location = 0) in vec2 texture_coordinates;
layout(location = 0) out vec4 display_color;

const uint dither_rgba8[16] = uint[](
    7U, 135U, 39U, 167U,
    199U, 71U, 231U, 103U,
    55U, 183U, 23U, 151U,
    247U, 119U, 215U, 87U);

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
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    uint dither_index = (pixel.y & 3U) * 4U + (pixel.x & 3U);
    float dither = (float(dither_rgba8[dither_index]) / 255.0 - 0.5) *
                   parameters.dither_scale;
    encoded += vec3(dither);
    vec3 output_rgb = parameters.srgb_destination != 0U
                          ? decode_srgb(encoded)
                          : encoded;
    display_color = vec4(output_rgb, source.a);
}
