cbuffer DrawMatrices : register(b0) {
    column_major float4x4 world;
    column_major float4x4 viewProjection;
};

struct VertexInput {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    float4 worldPosition = mul(world, float4(input.position, 1.0));
    output.position = mul(viewProjection, worldPosition);
    output.texcoord = input.texcoord;
    return output;
}

Texture2D txDiffuse : register(t0);
SamplerState samLinearShadow : register(s3);
cbuffer cbMaterial : register(b4) {
    float4 lighting;
    float4 emissiveAndAlphaRef;
};

void pixelMain(VertexOutput input) {
    clip(txDiffuse.Sample(samLinearShadow, input.texcoord).a -
         emissiveAndAlphaRef.w);
}
