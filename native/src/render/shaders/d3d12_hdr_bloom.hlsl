// Portable five-level bloom and bloom-aware tone-map shader.
// Keep this source synchronized with kD3d12BloomShader in d3d12_device.cpp.
Texture2D<float4> source_texture : register(t0);
Texture2D<float4> bloom_texture : register(t1);
SamplerState linear_clamp : register(s0);

cbuffer BloomParameters : register(b0) {
    uint mode;
    float exposure;
    float threshold;
    float remap;
    float texel_x;
    float texel_y;
    float sigma;
    uint sample_count;
    float composite_scale;
    float gamma_value;
    float saturation;
    float curve_scale;
    float curve_shoulder;
    uint srgb_destination;
    float dither_scale;
};

cbuffer BloomKernel : register(b1) {
    float4 kernel_offsets[4];
    float4 kernel_weights[4];
};

static const float dither_rgba8[16] = {
    7.0F, 135.0F, 39.0F, 167.0F, 199.0F, 71.0F, 231.0F, 103.0F,
    55.0F, 183.0F, 23.0F, 151.0F, 247.0F, 119.0F, 215.0F, 87.0F};

float decode_srgb_component(float encoded) {
    return encoded <= 0.04045F ? encoded / 12.92F :
                                 pow((encoded + 0.055F) / 1.055F, 2.4F);
}

float3 decode_srgb(float3 encoded) {
    return float3(decode_srgb_component(encoded.r),
                  decode_srgb_component(encoded.g),
                  decode_srgb_component(encoded.b));
}

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertex_id << 1U) & 2U, vertex_id & 2U);
    output.position = float4(output.uv * float2(2.0F, -2.0F) +
                             float2(-1.0F, 1.0F), 0.0F, 1.0F);
    return output;
}

float3 bright_pass(float3 rgb) {
    return min(max(rgb * exposure - threshold.xxx, 0.0F) * remap.xxx,
               64000.0F.xxx);
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    if (mode == 0U) {
        return float4(bright_pass(source_texture.SampleLevel(
                                      linear_clamp, input.uv, 0.0F).rgb), 1.0F);
    }
    if (mode == 1U) {
        float2 offset = float2(texel_x, texel_y) * 0.5F;
        float3 value = source_texture.SampleLevel(linear_clamp,
                                                  input.uv + float2(-offset.x, -offset.y), 0.0F).rgb;
        value += source_texture.SampleLevel(linear_clamp,
                                            input.uv + float2(offset.x, -offset.y), 0.0F).rgb;
        value += source_texture.SampleLevel(linear_clamp,
                                            input.uv + float2(-offset.x, offset.y), 0.0F).rgb;
        value += source_texture.SampleLevel(linear_clamp,
                                            input.uv + offset, 0.0F).rgb;
        return float4(value * 0.25F, 1.0F);
    }
    if (mode == 2U) {
        float3 value = 0.0F.xxx;
        for (uint tap = 0U; tap < 15U; ++tap) {
            if (tap >= sample_count) continue;
            uint vector_index = tap >> 2U;
            uint component_index = tap & 3U;
            float distance = kernel_offsets[vector_index][component_index];
            float weight = kernel_weights[vector_index][component_index];
            value += source_texture.SampleLevel(
                         linear_clamp, input.uv + float2(texel_x, texel_y) * distance,
                         0.0F).rgb * weight;
        }
        return float4(value, 1.0F);
    }
    if (mode == 3U) {
        return float4(min(source_texture.SampleLevel(
                              linear_clamp, input.uv, 0.0F).rgb * composite_scale,
                          64000.0F.xxx), 1.0F);
    }
    if (mode == 5U) {
        float4 source = source_texture.SampleLevel(linear_clamp, input.uv, 0.0F);
        float3 rgb = max(source.rgb, 0.0F);
        float luminance = dot(rgb, float3(0.2126F, 0.7152F, 0.0722F));
        float3 value = max(1.0F / 16384.0F,
                           lerp(luminance.xxx, rgb, saturation) * exposure);
        float3 decay = exp(-value * curve_scale);
        float3 shoulder = 1.0F - decay * curve_shoulder;
        float3 curve = saturate((1.0F - decay) * shoulder * shoulder);
        float3 mapped = curve;
        float3 glare = saturate(bloom_texture.SampleLevel(
            linear_clamp, input.uv, 0.0F).rgb);
        float3 encoded = pow(min(1.0F, mapped + (1.0F - mapped) * glare +
                                      1.0F / 4194304.0F),
                             1.0F / gamma_value);
        uint2 pixel = uint2(input.position.xy);
        uint dither_index = (pixel.y & 3U) * 4U + (pixel.x & 3U);
        encoded += (dither_rgba8[dither_index] / 255.0F - 0.5F) * dither_scale;
        return float4(srgb_destination != 0U ? decode_srgb(encoded) : encoded,
                      source.a);
    }
    float3 glare = saturate(source_texture.SampleLevel(
                                linear_clamp, input.uv, 0.0F).rgb);
    return float4(glare, 1.0F);
}
