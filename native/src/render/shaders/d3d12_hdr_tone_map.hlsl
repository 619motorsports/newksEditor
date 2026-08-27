// Source-evidenced curve approximation. The selected Yebis default also uses
// glare and dither inputs, which this first native pass does not implement.

Texture2D<float4> hdr_source : register(t0);
SamplerState point_sampler : register(s0);

cbuffer ToneMapParameters : register(b0) {
    float exposure;
    float gamma_value;
    float saturation;
    float curve_scale;
    float curve_shoulder;
    uint srgb_destination;
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
    return float4(srgb_destination != 0U ? decode_srgb(encoded) : encoded,
                  source.a);
}
