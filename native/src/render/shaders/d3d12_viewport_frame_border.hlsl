cbuffer DrawMatrices : register(b0) {
    column_major float4x4 world;
    column_major float4x4 viewProjection;
};

struct VertexInput {
    float3 position : POSITION;
    float4 color : COLOR;
};

struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR;
};

VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    float4 worldPosition = mul(world, float4(input.position, 1.0));
    output.position = mul(viewProjection, worldPosition);
    output.color = input.color;
    return output;
}

float4 pixelMain(VertexOutput input) : SV_Target {
    return input.color;
}
