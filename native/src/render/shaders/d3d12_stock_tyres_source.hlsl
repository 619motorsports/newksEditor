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
    output.bitangent = mul(world3, cross(input.normal, input.tangent));
    output.texcoord = input.texcoord;
    return output;
}

Texture2D txDiffuse : register(t0);
SamplerState samDiffuse : register(s1);
cbuffer StockTyresSourceMaterialConstants : register(b2) {
    float4 lighting;
    float4 emissiveAndAlpha;
};
cbuffer TyreFrame : register(b3) {
    float4 sunDirection;
    float4 sunColor;
    float4 ambientColor;
    float4 cameraPosition;
    float4 horizonColor;
    float4 skyColor;
    float4 fogColor;
    float4 fog;
};
Texture2D txNormal : register(t4);
SamplerState samNormal : register(s5);
Texture2D txDirty : register(t6);
SamplerState samDirty : register(s7);
Texture2D txBlur : register(t8);
SamplerState samBlur : register(s9);
Texture2D txNormalBlur : register(t10);
SamplerState samNormalBlur : register(s11);
TextureCube txCube : register(t21);
SamplerState samCube : register(s22);
// D3D shader-model 5 exposes only fourteen constant-buffer slots per register
// space. The backend's logical set-0 binding 23 is carried in space 1, slot 0.
cbuffer StockTyresSourceConstants : register(b0, space1) {
    float4 tyreControls;
    float4 reflection;
};

Texture2D txShadow0 : register(t16);
Texture2D txShadow1 : register(t17);
Texture2D txShadow2 : register(t18);
SamplerState shadowSampler : register(s19);
// Likewise, logical receiver binding 20 is space 1, slot 1.
cbuffer ShadowReceiver : register(b1, space1) {
    column_major float4x4 shadowMatrices[3];
    float4 splitDistances;
    float4 depthBiases;
    float4 shadowCameraPosition;
    float4 shadowCameraForward;
};

float3 applyFog(float3 surfaceColor, float3 worldPosition) {
    if (fog.z <= 0.5) return surfaceColor;
    float amount = pow(saturate(distance(cameraPosition.xyz, worldPosition) /
                              max(1.0, fog.x)), max(0.05, fog.y));
    return lerp(surfaceColor, fogColor.rgb, amount);
}

float3 tyreNormal(VertexOutput input, float3 texel, float3 blurred, float blurLevel) {
    float3 geometric = normalize(input.normal);
    float3 mapped = normalize(normalize(input.tangent) * texel.x +
                              normalize(input.bitangent) * texel.y + geometric * texel.z);
    float3 mappedBlur = normalize(normalize(input.tangent) * blurred.x +
                                  normalize(input.bitangent) * blurred.y + geometric * blurred.z);
    return lerp(mapped, mappedBlur, blurLevel);
}

float4 shadeTyre(VertexOutput input, float shadow) {
    float blurLevel = tyreControls.x;
    float dirtyLevel = tyreControls.y;
    float4 base = lerp(txDiffuse.Sample(samDiffuse, input.texcoord),
                       txBlur.Sample(samBlur, input.texcoord), blurLevel);
    float4 dirty = txDirty.Sample(samDirty, input.texcoord);
    base.rgb = lerp(base.rgb, dirty.rgb, dirty.a * dirtyLevel);
    float3 normalTexel = txNormal.Sample(samNormal, input.texcoord).rgb * 2.0 - 1.0;
    float3 normalBlurTexel = txNormalBlur.Sample(samNormalBlur, input.texcoord).rgb * 2.0 - 1.0;
    float3 n = tyreNormal(input, normalTexel, normalBlurTexel, blurLevel);
    float3 l = normalize(sunDirection.xyz);
    float3 v = normalize(cameraPosition.xyz - input.worldPosition);
    float ndl = max(dot(n, l), 0.0);
    float directSpecular = base.a * (1.0 - saturate(dirtyLevel)) * lighting.z;
    float3 specular = sunColor.rgb * (pow(max(dot(n, normalize(l + v)), 0.0),
                               max(1.0, lighting.w)) * directSpecular * shadow);
    float3 lit = max(base.rgb * (ambientColor.rgb * lighting.x +
                                 sunColor.rgb * lighting.y * ndl * shadow +
                                 emissiveAndAlpha.rgb) + specular, 0.0);
    float facing = 1.0 - max(dot(n, v), 0.0);
    float fresnel = min(tyreControls.z + pow(facing, max(tyreControls.w, 0.0)),
                        saturate(1.0 - dirtyLevel) * reflection.y);
    float mipLevel = 6.0 * saturate(1.0 - lighting.w / 255.0);
    float3 reflected = normalize(v - 2.0 * dot(v, n) * n);
    float3 reflectedColor = txCube.SampleLevel(samCube,
                                                float3(-reflected.x, reflected.y, reflected.z),
                                                mipLevel).rgb;
    float3 outputColor = reflection.x > 0.5
                             ? lit + fresnel * reflectedColor
                             : lerp(lit, reflectedColor, fresnel);
    return float4(max(applyFog(outputColor, input.worldPosition), 0.0), 1.0);
}

float4 pixelMain(VertexOutput input) : SV_Target {
    return shadeTyre(input, 1.0);
}

float shadowCascade(Texture2D shadowMap, float4x4 shadowMatrix, float bias,
                   float3 worldPosition, out bool inside) {
    float4 projected = mul(shadowMatrix, float4(worldPosition, 1.0));
    inside = false;
    if (projected.w <= 0.0) return 1.0;
    float3 coordinate = projected.xyz / projected.w;
    coordinate.xy = coordinate.xy * float2(0.5, -0.5) + float2(0.5, 0.5);
    if (coordinate.x <= 0.0 || coordinate.x >= 1.0 || coordinate.y <= 0.0 ||
        coordinate.y >= 1.0 || coordinate.z <= 0.0 || coordinate.z >= 1.0)
        return 1.0;
    inside = true;
    uint width = 0;
    uint height = 0;
    shadowMap.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float lit = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            lit += coordinate.z - bias <= shadowMap.SampleLevel(
                shadowSampler, coordinate.xy + float2(x, y) * texel, 0).r ? 1.0 : 0.0;
    return lit / 9.0;
}

float sunShadow(float3 worldPosition) {
    bool inside = false;
    float shadow = shadowCascade(txShadow0, shadowMatrices[0], depthBiases.x,
                                 worldPosition, inside);
    if (inside) return shadow;
    shadow = shadowCascade(txShadow1, shadowMatrices[1], depthBiases.y,
                           worldPosition, inside);
    if (inside) return shadow;
    shadow = shadowCascade(txShadow2, shadowMatrices[2], depthBiases.z,
                           worldPosition, inside);
    return inside ? shadow : 1.0;
}

float4 pixelShadowMain(VertexOutput input) : SV_Target {
    return shadeTyre(input, sunShadow(input.worldPosition));
}
