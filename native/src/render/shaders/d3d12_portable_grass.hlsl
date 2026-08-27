// Bounded expanded PortableGrass preview.  This is a source-labelled
// cross-backend approximation, not an exact CSP or native shader claim.

cbuffer PortableGrassConstants : register(b0) {
    float4x4 view_projection;
    float4 camera_position_fog;
    float4 sun_direction_fog_blend;
    float4 sun_color;
    float4 ambient_color_wetness;
    float4 fog_color_time;
    float4 wind_direction_strength;
};

Texture2D<float4> grass_atlas : register(t0);
SamplerState grass_sampler : register(s0);

struct GrassVertex {
    float3 position : POSITION0;
    float3 normal : NORMAL0;
    float2 atlas_uv : TEXCOORD0;
    float4 vertex_color : COLOR0;
    float vertex_wind : TEXCOORD1;
    float tip_weight : TEXCOORD2;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 texture_coordinates : TEXCOORD0;
    float4 blade_color : COLOR0;
    float3 blade_normal : NORMAL0;
    float3 world_position : TEXCOORD1;
};

VertexOutput vertex_main(GrassVertex input) {
    float3 displaced = input.position;
    float2 wind_direction = wind_direction_strength.xy;
    float wind_length = length(wind_direction);
    if (wind_length > 1.0e-8F)
        wind_direction /= wind_length;
    else
        wind_direction = float2(0.0F, 0.0F);

    // The expanded CPU layout supplies a tile-independent zero-to-one blade
    // tip weight. This keeps the portable fallback valid for multirow atlases.
    float blade_wind = saturate(input.vertex_wind) *
                       saturate(input.tip_weight);
    float wind_strength = min(max(wind_direction_strength.z, 0.0F), 1.0F);
    float phase = fog_color_time.w * 1.7F +
                  dot(input.position.xz, wind_direction) * 0.09F;
    float displacement = sin(phase) * 0.04F * wind_strength * blade_wind;
    displaced.xz += wind_direction * displacement;

    VertexOutput output;
    output.texture_coordinates = input.atlas_uv;
    output.blade_color = input.vertex_color;
    output.blade_normal = input.normal;
    output.world_position = displaced;
    output.position = mul(view_projection, float4(displaced, 1.0F));
    return output;
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    float4 atlas_sample = grass_atlas.Sample(grass_sampler,
                                              input.texture_coordinates);
    float alpha = atlas_sample.a * saturate(input.blade_color.a);
    if (alpha < 0.5F)
        discard;

    float3 normal = normalize(input.blade_normal);
    float3 sun_direction = normalize(sun_direction_fog_blend.xyz);
    float sun_diffuse = max(dot(normal, sun_direction), 0.0F);
    float3 albedo = max(atlas_sample.rgb * input.blade_color.rgb,
                        float3(0.0F, 0.0F, 0.0F));
    float wetness = saturate(ambient_color_wetness.w);
    albedo *= lerp(1.0F, pow(0.65F, 2.2F), wetness);
    float3 lit = albedo * (ambient_color_wetness.xyz * 0.35F +
                           sun_color.xyz * 0.8F * sun_diffuse);

    float distance_to_camera = distance(camera_position_fog.xyz,
                                        input.world_position);
    float fog_range = max(camera_position_fog.w, 1.0F);
    float fog_amount = pow(saturate(distance_to_camera / fog_range),
                           max(0.05F, sun_direction_fog_blend.w));
    lit = lerp(lit, fog_color_time.xyz, fog_amount);
    return float4(max(lit, float3(0.0F, 0.0F, 0.0F)), alpha);
}
