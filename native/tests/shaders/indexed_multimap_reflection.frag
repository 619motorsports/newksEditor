#version 450

layout(set = 0, binding = 21) uniform textureCube reflectionCube;
layout(set = 0, binding = 22) uniform sampler reflectionSampler;
layout(set = 0, binding = 23) uniform ReflectionConstants {
  vec4 fresnelAndAdditive;
} reflection;

layout(location = 0) out vec4 color;

void main() {
  color = texture(samplerCube(reflectionCube, reflectionSampler),
                  vec3(1.0, 0.0, 0.0)) *
          reflection.fresnelAndAdditive;
}
