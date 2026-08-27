#version 450

// Source authority: installed ksPerPixelMultiMap_damage_dirt.shader container,
// SHA-256 76d6a625c34e386641667a44a0433c6d1dc2af5be9e4a8ebbd6a886181f97dc8.
// Its pixel FXO has SHA-256 ef7835b917eddf0b6e233d05ef8d66126e20bf4ca828b429b742dca3552d7743.
// This fixture implements only the recovered bounded dirt == 0 damage stage.
layout(location = 0) in vec3 fragmentNormal;
layout(location = 1) in vec3 fragmentTangent;
layout(location = 2) in vec3 fragmentBitangent;
layout(location = 3) in vec3 fragmentWorld;
layout(location = 4) in vec2 fragmentTexcoord;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform texture2D diffuseTexture;
layout(set = 0, binding = 1) uniform sampler diffuseSampler;
layout(std140, set = 0, binding = 2) uniform KsPerPixelMaterial {
    vec4 lighting;
    vec4 fresnel;
    vec4 emissive;
    vec4 detail;
    vec4 damageZones;
} material;
layout(std140, set = 0, binding = 3) uniform KsPerPixelFrame {
    vec4 sun_direction;
    vec4 sun_color;
    vec4 ambient_color;
    vec4 camera_position;
    vec4 horizon_color;
    vec4 sky_color;
    vec4 fog_color;
    vec4 fog;
} frame;
layout(set = 0, binding = 4) uniform texture2D normalTexture;
layout(set = 0, binding = 5) uniform sampler normalSampler;
layout(set = 0, binding = 6) uniform texture2D mapsTexture;
layout(set = 0, binding = 7) uniform sampler mapsSampler;
layout(set = 0, binding = 12) uniform texture2D damageTexture;
layout(set = 0, binding = 13) uniform sampler damageSampler;
layout(set = 0, binding = 14) uniform texture2D damageMaskTexture;
layout(set = 0, binding = 15) uniform sampler damageMaskSampler;

void main() {
    vec4 diffuseTexel = texture(sampler2D(diffuseTexture, diffuseSampler), fragmentTexcoord);
    vec4 normalTexel = texture(sampler2D(normalTexture, normalSampler), fragmentTexcoord);
    vec3 sampledNormal = normalTexel.rgb * 2.0 - 1.0;
    vec3 n = normalize(normalize(fragmentTangent) * sampledNormal.x +
                       normalize(fragmentBitangent) * sampledNormal.y +
                       normalize(fragmentNormal) * sampledNormal.z);
    vec3 maps = texture(sampler2D(mapsTexture, mapsSampler), fragmentTexcoord).rgb;
    vec4 damageTexel = texture(sampler2D(damageTexture, damageSampler), fragmentTexcoord);
    vec4 damageMask = texture(sampler2D(damageMaskTexture, damageMaskSampler), fragmentTexcoord);
    float amount = clamp(damageTexel.a * dot(damageMask, material.damageZones), 0.0, 1.0);
    vec3 surface = mix(diffuseTexel.rgb, damageTexel.rgb, amount);
    float mappedSpecular = material.lighting.z * maps.r * (1.0 - amount) *
                           (1.0 + amount * (normalTexel.a - 1.0));
    float mappedPower = max(1.0, material.lighting.w * maps.g + 1.0);
    vec3 l = normalize(frame.sun_direction.xyz);
    vec3 v = normalize(frame.camera_position.xyz - fragmentWorld);
    float ndl = max(dot(n, l), 0.0);
    vec3 diffuse = surface *
                   (frame.ambient_color.rgb * material.lighting.x +
                    frame.sun_color.rgb * material.lighting.y * ndl);
    vec3 spec = frame.sun_color.rgb *
                (pow(max(dot(n, normalize(l + v)), 0.0), mappedPower) * mappedSpecular);
    vec3 lit = max(diffuse + spec + surface * material.emissive.rgb, vec3(0.0));
    float fogAmount = frame.fog.z > 0.5
        ? pow(clamp(distance(frame.camera_position.xyz, fragmentWorld) /
                        max(1.0, frame.fog.x), 0.0, 1.0),
              max(0.05, frame.fog.y))
        : 0.0;
    color = vec4(mix(lit, frame.fog_color.rgb, fogAmount), diffuseTexel.a);
}
