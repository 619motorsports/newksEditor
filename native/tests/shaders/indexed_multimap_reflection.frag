#version 450

layout(set = 0, binding = 21) uniform textureCube reflectionCube;
layout(set = 0, binding = 22) uniform sampler reflectionSampler;
layout(set = 0, binding = 23) uniform ReflectionConstants {
  vec4 fresnelAndAdditive;
} reflection;

layout(location = 0) out vec4 color;

void main() {
  // Source authority: installed ksPerPixelMultiMap_ps.fxo DXBC, SHA-256
  // b5cc236cef21d3d01f7a80510e64b7a9a21bf5727c0f5a0b93b6e2fdf1f18a58.
  // Direct lighting and maps channels are fixed so the backend test isolates
  // the portable cube and reflection-constant bindings.
  const vec3 normal = vec3(-1.0, 0.0, 0.0);
  const vec3 view = vec3(1.0, 0.0, 0.0);
  const float mapsSpecularExponent = 0.5;
  const float ksSpecularExponent = 255.0;
  const float mapsReflection = 0.8;
  const vec3 lit = vec3(0.2);

  float branch = reflection.fresnelAndAdditive.w;
  float roughnessDivisor = branch == 2.0 ? 8.0 : 255.0;
  float mipLevel = 6.0 * clamp(
      1.0 - mapsSpecularExponent * ksSpecularExponent / roughnessDivisor,
      0.0, 1.0);
  vec3 reflected = normalize(view - 2.0 * dot(view, normal) * normal);
  vec3 cubeDirection = vec3(-reflected.x, reflected.y, reflected.z);
  vec3 reflectedColor = mapsReflection * textureLod(
      samplerCube(reflectionCube, reflectionSampler), cubeDirection,
      mipLevel).rgb;

  float grazing = clamp(1.0 + dot(normal, view), 0.0, 1.0);
  float exponent = branch == 1.0 || branch == 2.0
                       ? reflection.fresnelAndAdditive.y
                       : max(reflection.fresnelAndAdditive.y, 1.0);
  float anglePower = grazing > 0.0 ? pow(grazing, exponent) : 0.0;
  float fresnel = min(reflection.fresnelAndAdditive.x + anglePower,
                      reflection.fresnelAndAdditive.z);
  color = branch == 1.0
              ? vec4(lit + fresnel * reflectedColor, 2.0)
              : vec4(mix(lit, reflectedColor, fresnel), 1.0);
}
