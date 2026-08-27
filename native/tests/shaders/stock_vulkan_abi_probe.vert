#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec3 inTangent;

layout(set = 0, binding = 0) uniform CameraProbe { vec4 value; } cameraProbe;
layout(set = 0, binding = 1) uniform ObjectProbe { vec4 value; } objectProbe;
layout(set = 0, binding = 2) uniform LightingProbeVs { vec4 value; } lightingProbe;
layout(set = 0, binding = 3) uniform ShadowsProbeVs { vec4 value; } shadowsProbe;

layout(location = 0) out vec2 outUv;

void main() {
    vec4 descriptorProbe = cameraProbe.value + objectProbe.value +
                           lightingProbe.value + shadowsProbe.value;
    vec3 vertexProbe = inNormal + inTangent;
    gl_Position = vec4(inPosition + descriptorProbe.xyz + vertexProbe * descriptorProbe.w, 1.0);
    outUv = inUv;
}
