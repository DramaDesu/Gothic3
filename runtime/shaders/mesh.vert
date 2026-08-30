#version 450

// Skinning happens here: vertices arrive in bind pose with their bone bindings,
// and the only thing that changes per frame is the matrix buffer.

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 lightDirection;
    uint boneBase;
}
push;

layout(set = 0, binding = 2) readonly buffer Bones
{
    mat4 matrices[];
}
bones;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in uvec4 inBones;
layout(location = 4) in vec4 inWeights;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;

void main()
{
    // Weights are normalised on the way in, so a plain sum is enough. Each
    // piece of the character indexes its own slice of the matrix buffer.
    mat4 skin = inWeights.x * bones.matrices[push.boneBase + inBones.x] +
                inWeights.y * bones.matrices[push.boneBase + inBones.y] +
                inWeights.z * bones.matrices[push.boneBase + inBones.z] +
                inWeights.w * bones.matrices[push.boneBase + inBones.w];

    vec4 posed = skin * vec4(inPosition, 1.0);

    outNormal = mat3(skin) * inNormal;
    outTexCoord = inTexCoord;
    gl_Position = push.viewProjection * posed;
}
