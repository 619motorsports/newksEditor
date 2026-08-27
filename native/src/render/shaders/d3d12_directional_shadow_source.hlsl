cbuffer DrawMatrices : register(b0) {
    column_major float4x4 world;
    column_major float4x4 viewProjection;
};

struct VertexInput {
    float3 position : POSITION;
};

float4 vertexMain(VertexInput input) : SV_Position {
    float4 worldPosition = mul(world, float4(input.position, 1.0));
    return mul(viewProjection, worldPosition);
}
