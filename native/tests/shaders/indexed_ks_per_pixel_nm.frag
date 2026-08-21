#version 450

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
} material;
layout(std140, set = 0, binding = 3) uniform KsPerPixelFrame {
    vec4 sun_direction;
    vec4 sun_color;
    vec4 ambient_color;
    vec4 camera_position;
} frame;
layout(set = 0, binding = 4) uniform texture2D normalTexture;
layout(set = 0, binding = 5) uniform sampler normalSampler;

void main() {
    vec4 diffuseTexel = texture(sampler2D(diffuseTexture, diffuseSampler), fragmentTexcoord);
    vec4 sampledNormal = texture(sampler2D(normalTexture, normalSampler), fragmentTexcoord);
    vec3 geometricNormal = normalize(fragmentNormal);
    vec3 tangentNormal = sampledNormal.rgb * 2.0 - 1.0;
    vec3 n = normalize(normalize(fragmentTangent) * tangentNormal.x +
                       normalize(fragmentBitangent) * tangentNormal.y +
                       geometricNormal * tangentNormal.z);
    vec3 maps = vec3(1.0);
    float mappedSpecular = material.lighting.z * maps.r;
    float mappedPower = max(1.0, material.lighting.w * maps.g + 1.0);
    vec3 l = normalize(frame.sun_direction.xyz);
    vec3 v = normalize(frame.camera_position.xyz - fragmentWorld);
    float ndl = max(dot(n, l), 0.0);
    vec3 diffuse = diffuseTexel.rgb *
                   (frame.ambient_color.rgb * material.lighting.x +
                    frame.sun_color.rgb * material.lighting.y * ndl);
    vec3 spec = frame.sun_color.rgb *
                (pow(max(dot(n, normalize(l + v)), 0.0), mappedPower) *
                 mappedSpecular);
    color = vec4(max(diffuse + spec + diffuseTexel.rgb * material.emissive.rgb,
                     vec3(0.0)), diffuseTexel.a);
}
