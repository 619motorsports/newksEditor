#version 450

// Source-equivalent ksTyres pixel path. Installed-bytecode parity is not
// claimed; the recovered texture operations and reflection branches are the
// authority for this bounded translation.
layout(location = 0) in vec3 fragmentNormal;
layout(location = 1) in vec3 fragmentTangent;
layout(location = 2) in vec3 fragmentBitangent;
layout(location = 3) in vec3 fragmentWorld;
layout(location = 4) in vec2 fragmentTexcoord;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform texture2D txDiffuse;
layout(set = 0, binding = 1) uniform sampler samDiffuse;
layout(std140, set = 0, binding = 2) uniform TyreMaterial {
    vec4 lighting; // ambient, diffuse, specular, specular exponent
    vec4 emissive;
} material;
layout(std140, set = 0, binding = 3) uniform TyreFrame {
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 cameraPosition;
    vec4 horizonColor;
    vec4 skyColor;
    vec4 fogColor;
    vec4 fog;
} frame;
layout(set = 0, binding = 4) uniform texture2D txNormal;
layout(set = 0, binding = 5) uniform sampler samNormal;
layout(set = 0, binding = 6) uniform texture2D txDirty;
layout(set = 0, binding = 7) uniform sampler samDirty;
layout(set = 0, binding = 8) uniform texture2D txBlur;
layout(set = 0, binding = 9) uniform sampler samBlur;
layout(set = 0, binding = 10) uniform texture2D txNormalBlur;
layout(set = 0, binding = 11) uniform sampler samNormalBlur;
layout(set = 0, binding = 21) uniform textureCube txCube;
layout(set = 0, binding = 22) uniform sampler samCube;
layout(std140, set = 0, binding = 23) uniform ReflectionConstants {
    vec4 tyreControls; // blurLevel, dirtyLevel, fresnelC, fresnelEXP
    vec4 reflection; // isAdditive, fresnelMaxLevel, reserved, reserved
} reflection;

vec3 applyFog(vec3 surfaceColor) {
    if (frame.fog.z <= 0.5) return surfaceColor;
    float amount = pow(clamp(distance(frame.cameraPosition.xyz, fragmentWorld) /
                             max(1.0, frame.fog.x), 0.0, 1.0),
                       max(0.05, frame.fog.y));
    return mix(surfaceColor, frame.fogColor.rgb, amount);
}

void main() {
    float blurLevel = reflection.tyreControls.x;
    float dirtyLevel = reflection.tyreControls.y;
    vec4 base = mix(texture(sampler2D(txDiffuse, samDiffuse), fragmentTexcoord),
                    texture(sampler2D(txBlur, samBlur), fragmentTexcoord),
                    blurLevel);
    vec4 dirty = texture(sampler2D(txDirty, samDirty), fragmentTexcoord);
    base.rgb = mix(base.rgb, dirty.rgb, dirty.a * dirtyLevel);

    vec3 normalTexel = texture(sampler2D(txNormal, samNormal), fragmentTexcoord).rgb * 2.0 - 1.0;
    vec3 normalBlurTexel = texture(sampler2D(txNormalBlur, samNormalBlur), fragmentTexcoord).rgb * 2.0 - 1.0;
    vec3 geometric = normalize(fragmentNormal);
    vec3 mapped = normalize(normalize(fragmentTangent) * normalTexel.x +
                            normalize(fragmentBitangent) * normalTexel.y +
                            geometric * normalTexel.z);
    vec3 mappedBlur = normalize(normalize(fragmentTangent) * normalBlurTexel.x +
                                normalize(fragmentBitangent) * normalBlurTexel.y +
                                geometric * normalBlurTexel.z);
    vec3 n = mix(mapped, mappedBlur, blurLevel);

    vec3 l = normalize(frame.sunDirection.xyz);
    vec3 v = normalize(frame.cameraPosition.xyz - fragmentWorld);
    float ndl = max(dot(n, l), 0.0);
    float directSpecular = base.a * (1.0 - clamp(dirtyLevel, 0.0, 1.0)) * material.lighting.z;
    vec3 specular = frame.sunColor.rgb *
                    (pow(max(dot(n, normalize(l + v)), 0.0),
                         max(1.0, material.lighting.w)) * directSpecular);
    vec3 lit = max(base.rgb * (frame.ambientColor.rgb * material.lighting.x +
                               frame.sunColor.rgb * material.lighting.y * ndl +
                               material.emissive.rgb) + specular, vec3(0.0));

    float facing = 1.0 - max(dot(n, v), 0.0);
    float fresnel = min(reflection.tyreControls.z +
                        pow(facing, max(reflection.tyreControls.w, 0.0)),
                        clamp(1.0 - dirtyLevel, 0.0, 1.0) *
                            reflection.reflection.y);
    float mipLevel = 6.0 * clamp(1.0 - material.lighting.w / 255.0, 0.0, 1.0);
    vec3 reflected = normalize(v - 2.0 * dot(v, n) * n);
    vec3 reflectedColor = textureLod(samplerCube(txCube, samCube),
                                     vec3(-reflected.x, reflected.y, reflected.z), mipLevel).rgb;
    vec3 outputColor = reflection.reflection.x > 0.5
                           ? lit + fresnel * reflectedColor
                           : mix(lit, reflectedColor, fresnel);
    color = vec4(max(applyFog(outputColor), vec3(0.0)), 1.0);
}
