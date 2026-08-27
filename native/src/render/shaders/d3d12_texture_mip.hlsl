// Source-evidenced linear downsample shader for D3D12 texture mip generation.
//
// The installed editor delegates cube filtering to D3D11 GenerateMips. Its
// driver-specific kernel is not present in the editor binary, so this shader
// is a portable linear-filter implementation, not a claim of bit-exact native
// filtering.

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

Texture2DArray<float4> source_texture : register(t0);
SamplerState linear_clamp : register(s0);

float4 pixel_main(VertexOutput input) : SV_Target {
    return source_texture.SampleLevel(linear_clamp,
                                      float3(input.uv, 0.0F), 0.0F);
}
