// WebGL-aligned portable cloud billboards and shading. This is not an exact
// native ksClouds shader reconstruction. The recovered native evidence proves
// cloud participation during cube capture, while public/app.js supplies the
// placement and pixel equations.

cbuffer CloudConstants : register(b0) {
    float4x4 view_projection;
    float4 camera_position_time;
    float4 light_direction_fog;
    float4 light_color_cover;
    float4 ambient_color_cutoff;
    float4 cloud_color;
};

Texture2D<float4> cloud_texture : register(t0);
SamplerState linear_repeat : register(s0);

struct CloudVertex {
    float2 corner : TEXCOORD0;
    float3 cloud_offset : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float cloud_speed : TEXCOORD3;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 texture_coordinates : TEXCOORD0;
};

VertexOutput vertex_main(CloudVertex input) {
    float angle = input.cloud_speed * camera_position_time.w;
    float cosine = cos(angle);
    float sine = sin(angle);
    float3 offset = float3(input.cloud_offset.x * cosine -
                               input.cloud_offset.z * sine,
                           input.cloud_offset.y,
                           input.cloud_offset.x * sine +
                               input.cloud_offset.z * cosine);

    // The native billboard uses the local normal (0, -1, 0). The fallback
    // keeps the portable pass finite when a bounded layout reaches the axis.
    float3 heading = normalize(offset);
    float3 right = cross(heading, float3(0.0F, -1.0F, 0.0F));
    if (dot(right, right) < 1.0e-8F)
        right = cross(heading, float3(0.0F, 0.0F, 1.0F));
    right = normalize(right);
    float3 up = normalize(cross(heading, right));

    float3 position = camera_position_time.xyz + offset +
                      right * input.corner.x + up * input.corner.y;
    VertexOutput output;
    output.texture_coordinates = input.uv;
    output.position = mul(view_projection, float4(position, 1.0F));
    return output;
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    float fog = clamp(900.0F / max(light_direction_fog.w, 1.0e-7F),
                      0.0F, 1.0F);
    float4 texture_sample = cloud_texture.Sample(linear_repeat,
                                                   input.texture_coordinates);
    float value = dot(float3(0.0F, -1.0F, 0.0F),
                      light_direction_fog.xyz) *
                  (1.0F - texture_sample.r) *
                  cloud_color.x;
    float3 color = 0.5F * (light_color_cover.xyz +
                           ambient_color_cutoff.xyz) *
                   (1.0F - ambient_color_cutoff.w);
    color = lerp(value.xxx, (ambient_color_cutoff.x * 0.75F).xxx, fog) *
            ambient_color_cutoff.w + color;
    float alpha = saturate(light_color_cover.w * texture_sample.a);
    return float4(color, alpha);
}
