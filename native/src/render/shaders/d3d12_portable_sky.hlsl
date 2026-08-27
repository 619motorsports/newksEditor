// Portable WebGL-aligned sky background. This is deliberately not a
// recovered native SkyBox shader or register contract.

cbuffer PortableSky : register(b0) {
    float4 camera_forward_tan_half_fov;
    float4 camera_right_aspect;
    float4 camera_up;
    float4 horizon_color;
    float4 sky_color;
    float4 sun_color;
    float4 sun_direction;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    // D3D's viewport maps positive NDC Y to the top of the render target.
    // Carry that top-of-screen orientation into the WebGL ray formula.
    output.ndc = vertex_id == 0U ? float2(-1.0F, 1.0F) :
                 vertex_id == 1U ? float2(3.0F, 1.0F) :
                                   float2(-1.0F, -3.0F);
    output.position = float4(output.ndc, 0.0F, 1.0F);
    return output;
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    // Matches public/app.js:2188-2191. The camera basis and sun direction
    // are normalized by build_portable_sky_shader_constants().
    float tan_half_fov = camera_forward_tan_half_fov.w;
    float aspect = camera_right_aspect.w;
    float3 ray = normalize(
        camera_forward_tan_half_fov.xyz +
        camera_right_aspect.xyz * input.ndc.x * tan_half_fov * aspect +
        camera_up.xyz * input.ndc.y * tan_half_fov);
    float height = smoothstep(-0.08F, 0.42F, ray.y);
    float3 color = lerp(horizon_color.xyz, sky_color.xyz, height);
    float sun = smoothstep(cos(0.02618F), cos(0.00524F),
                           dot(ray, normalize(sun_direction.xyz)));
    color += sun_color.xyz * sun;
    return float4(max(color, 0.0F.xxx), 1.0F);
}
