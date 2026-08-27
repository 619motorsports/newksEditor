#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord;
layout(location = 3) in vec3 tangent;
layout(location = 4) in vec4 boneWeights;
layout(location = 5) in vec4 boneIndices;

layout(location = 0) out vec3 fragmentNormal;
layout(location = 1) out vec3 fragmentWorld;
layout(location = 2) out vec2 fragmentTexcoord;
layout(location = 3) flat out vec4 transportedBoneWeights;
layout(location = 4) flat out vec4 transportedBoneIndices;
layout(location = 5) out vec3 transportedTangent;

layout(push_constant) uniform DrawMatrices {
    mat4 world;
    mat4 viewProjection;
} drawMatrices;

void main() {
    vec4 worldPosition = drawMatrices.world * vec4(position, 1.0);
    fragmentNormal = mat3(drawMatrices.world) * normal;
    fragmentWorld = worldPosition.xyz;
    fragmentTexcoord = texcoord;
    transportedBoneWeights = boneWeights;
    transportedBoneIndices = boneIndices;
    transportedTangent = mat3(drawMatrices.world) * tangent;
    gl_Position = drawMatrices.viewProjection * worldPosition;
}
