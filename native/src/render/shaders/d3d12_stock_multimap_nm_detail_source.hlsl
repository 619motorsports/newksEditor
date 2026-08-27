cbuffer DrawMatrices : register(b0) {
    column_major float4x4 world;
    column_major float4x4 viewProjection;
};

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
    float3 tangent : TANGENT;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float3 bitangent : TEXCOORD3;
    float3 worldPosition : TEXCOORD4;
    float2 texcoord : TEXCOORD0;
};

VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    float4 positionWorld = mul(world, float4(input.position, 1.0));
    float3x3 world3 = (float3x3)world;
    output.position = mul(viewProjection, positionWorld);
    output.worldPosition = positionWorld.xyz;
    output.normal = mul(world3, input.normal);
    output.tangent = mul(world3, input.tangent);
    output.bitangent = mul(world3, cross(input.tangent, input.normal));
    output.texcoord = input.texcoord;
    return output;
}

Texture2D diffuseTexture : register(t0);
SamplerState diffuseSampler : register(s1);
cbuffer KsPerPixelMaterial : register(b2) {
    float4 lighting;
    float4 fresnel;
    float4 emissive;
    float4 detail;
};
cbuffer KsPerPixelFrame : register(b3) {
    float4 sun_direction;
    float4 sun_color;
    float4 ambient_color;
    float4 camera_position;
    float4 horizon_color;
    float4 sky_color;
    float4 fog_color;
    float4 fog;
};
Texture2D normalTexture : register(t4);
SamplerState normalSampler : register(s5);
Texture2D mapsTexture : register(t6);
SamplerState mapsSampler : register(s7);
Texture2D detailTexture : register(t8);
SamplerState detailSampler : register(s9);
Texture2D normalDetailTexture : register(t10);
SamplerState normalDetailSampler : register(s11);

float4 pixelMain(VertexOutput input) : SV_Target {
    float4 diffuseTexel = diffuseTexture.Sample(diffuseSampler, input.texcoord);
    float detailMask = detail.x > 0.5 ? 1.0 - diffuseTexel.a : 0.0;
    float4 detailTexel = detailTexture.Sample(
        detailSampler, input.texcoord * max(detail.y, 0.0001));
    float4 texel = lerp(diffuseTexel, diffuseTexel * detailTexel, detailMask);

    float3 sampledNormal = normalTexture.Sample(normalSampler, input.texcoord).rgb * 2.0 - 1.0;
    float3 geometricNormal = normalize(input.normal);
    float3 n = normalize(normalize(input.tangent) * sampledNormal.x +
                         normalize(input.bitangent) * sampledNormal.y +
                         geometricNormal * sampledNormal.z);
    if (detail.x > 0.5) {
        float3 detailNormal = normalDetailTexture.Sample(
            normalDetailSampler,
            input.texcoord * max(detail.y, 0.0001)).rgb * 2.0 - 1.0;
        float3 detailWorld = normalize(normalize(input.tangent) * detailNormal.x +
                                       normalize(input.bitangent) * detailNormal.y +
                                       n * detailNormal.z);
        n = normalize(lerp(n, detailWorld,
                           saturate(detailMask * detail.z)));
    }

    float3 maps = mapsTexture.Sample(mapsSampler, input.texcoord).rgb;
    float mappedSpecular = lighting.z * maps.r;
    mappedSpecular = lerp(mappedSpecular, mappedSpecular * detailTexel.a,
                          detailMask);
    float mappedPower = max(1.0, lighting.w * maps.g + 1.0);
    float3 l = normalize(sun_direction.xyz);
    float3 v = normalize(camera_position.xyz - input.worldPosition);
    float ndl = max(dot(n, l), 0.0);
    float3 diffuse = texel.rgb *
                     (ambient_color.rgb * lighting.x +
                      sun_color.rgb * lighting.y * ndl);
    float3 specular = sun_color.rgb *
                      (pow(max(dot(n, normalize(l + v)), 0.0), mappedPower) *
                       mappedSpecular);
    float3 lit = max(diffuse + specular + texel.rgb * emissive.rgb, 0.0);
    float fogAmount = fog.z > 0.5
        ? pow(saturate(distance(camera_position.xyz, input.worldPosition) /
                       max(1.0, fog.x)), max(0.05, fog.y))
        : 0.0;
    return float4(lerp(lit, fog_color.rgb, fogAmount), texel.a);
}
