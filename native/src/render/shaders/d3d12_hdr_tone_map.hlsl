// Source-evidenced Yebis curve approximation and fixed-phase 4x4 RGBA8 dither.
// Glare remains a separate pass.

Texture2D<float4> hdr_source : register(t0);
SamplerState point_sampler : register(s0);

cbuffer ToneMapParameters : register(b0) {
    float exposure;
    float gamma_value;
    float saturation;
    float curve_scale;
    float curve_shoulder;
    uint srgb_destination;
    float dither_scale;
};

static const float dither_rgba8[16] = {
    7.0F, 135.0F, 39.0F, 167.0F,
    199.0F, 71.0F, 231.0F, 103.0F,
    55.0F, 183.0F, 23.0F, 151.0F,
    247.0F, 119.0F, 215.0F, 87.0F
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 texture_coordinates : TEXCOORD0;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 position = float2((vertex_id << 1U) & 2U, vertex_id & 2U);
    output.texture_coordinates = position;
    output.position = float4(position * float2(2.0F, -2.0F) +
                                 float2(-1.0F, 1.0F),
                             0.0F, 1.0F);
    return output;
}

float decode_srgb_component(float encoded) {
    return encoded <= 0.04045F
               ? encoded / 12.92F
               : pow((encoded + 0.055F) / 1.055F, 2.4F);
}

float3 decode_srgb(float3 encoded) {
    return float3(decode_srgb_component(encoded.r),
                  decode_srgb_component(encoded.g),
                  decode_srgb_component(encoded.b));
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    float4 source = hdr_source.SampleLevel(
        point_sampler, input.texture_coordinates, 0.0F);
    float3 rgb = max(source.rgb, 0.0F);
    float luminance = dot(rgb, float3(0.2126F, 0.7152F, 0.0722F));
    float3 value = max(1.0F / 16384.0F,
                       lerp(luminance.xxx, rgb, saturation) * exposure);
    float3 decay = exp(-value * curve_scale);
    float3 shoulder = 1.0F - decay * curve_shoulder;
    float3 curve = saturate((1.0F - decay) * shoulder * shoulder);
    float3 encoded = pow(min(1.0F, curve + 1.0F / 4194304.0F),
                         1.0F / gamma_value);
    uint2 pixel = uint2(input.position.xy);
    uint dither_index = (pixel.y & 3U) * 4U + (pixel.x & 3U);
    encoded += (dither_rgba8[dither_index] / 255.0F - 0.5F) * dither_scale;
    return float4(srgb_destination != 0U ? decode_srgb(encoded) : encoded,
                  source.a);
}
