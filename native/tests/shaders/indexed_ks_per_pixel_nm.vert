#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord;
layout(location = 3) in vec3 tangent;

layout(location = 0) out vec3 fragmentNormal;
layout(location = 1) out vec3 fragmentTangent;
layout(location = 2) out vec3 fragmentBitangent;
layout(location = 3) out vec3 fragmentWorld;
layout(location = 4) out vec2 fragmentTexcoord;

layout(push_constant) uniform DrawMatrices {
    mat4 world;
    mat4 viewProjection;
} drawMatrices;

void main() {
    vec4 worldPosition = drawMatrices.world * vec4(position, 1.0);
    mat3 world3 = mat3(drawMatrices.world);
    fragmentWorld = worldPosition.xyz;
    fragmentNormal = world3 * normal;
    fragmentTangent = world3 * tangent;
    fragmentBitangent = world3 * cross(tangent, normal);
    fragmentTexcoord = texcoord;
    gl_Position = drawMatrices.viewProjection * worldPosition;
}
