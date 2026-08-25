#version 450

// Exact generic detail stack from public/app.js:2074, 2077, and 2080.
// Reflection/Fresnel and all weather/overlay branches are intentionally
// excluded from this bounded execution fixture.
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
layout(set = 0, binding = 8) uniform texture2D detailTexture;
layout(set = 0, binding = 9) uniform sampler detailSampler;
layout(set = 0, binding = 10) uniform texture2D normalDetailTexture;
layout(set = 0, binding = 11) uniform sampler normalDetailSampler;

void main() {
    vec4 diffuseTexel = texture(sampler2D(diffuseTexture, diffuseSampler), fragmentTexcoord);
    float detailMask = material.detail.x > 0.5 ? 1.0 - diffuseTexel.a : 0.0;
    vec4 detailTexel = texture(
        sampler2D(detailTexture, detailSampler),
        fragmentTexcoord * max(material.detail.y, 0.0001));
    vec4 texel = mix(diffuseTexel, diffuseTexel * detailTexel, detailMask);

    vec3 sampledNormal = texture(sampler2D(normalTexture, normalSampler), fragmentTexcoord).rgb * 2.0 - 1.0;
    vec3 geometricNormal = normalize(fragmentNormal);
    vec3 tangentNormal = sampledNormal;
    vec3 n = normalize(normalize(fragmentTangent) * tangentNormal.x +
                       normalize(fragmentBitangent) * tangentNormal.y +
                       geometricNormal * tangentNormal.z);
    if (material.detail.x > 0.5) {
        vec3 detailNormal = texture(
            sampler2D(normalDetailTexture, normalDetailSampler),
            fragmentTexcoord * max(material.detail.y, 0.0001)).rgb * 2.0 - 1.0;
        vec3 detailWorld = normalize(normalize(fragmentTangent) * detailNormal.x +
                                     normalize(fragmentBitangent) * detailNormal.y +
                                     n * detailNormal.z);
        n = normalize(mix(n, detailWorld,
                          clamp(detailMask * material.detail.z, 0.0, 1.0)));
    }

    vec3 maps = texture(sampler2D(mapsTexture, mapsSampler), fragmentTexcoord).rgb;
    float mappedSpecular = material.lighting.z * maps.r;
    mappedSpecular = mix(mappedSpecular, mappedSpecular * detailTexel.a, detailMask);
    float mappedPower = max(1.0, material.lighting.w * maps.g + 1.0);
    vec3 l = normalize(frame.sun_direction.xyz);
    vec3 v = normalize(frame.camera_position.xyz - fragmentWorld);
    float ndl = max(dot(n, l), 0.0);
    vec3 diffuse = texel.rgb *
                   (frame.ambient_color.rgb * material.lighting.x +
                    frame.sun_color.rgb * material.lighting.y * ndl);
    vec3 spec = frame.sun_color.rgb *
                (pow(max(dot(n, normalize(l + v)), 0.0), mappedPower) *
                 mappedSpecular);
    vec3 lit = max(diffuse + spec + texel.rgb * material.emissive.rgb,
                   vec3(0.0));
    float fogAmount = frame.fog.z > 0.5
        ? pow(clamp(distance(frame.camera_position.xyz, fragmentWorld) /
                        max(1.0, frame.fog.x), 0.0, 1.0),
              max(0.05, frame.fog.y))
        : 0.0;
    color = vec4(mix(lit, frame.fog_color.rgb, fogAmount), texel.a);
}
