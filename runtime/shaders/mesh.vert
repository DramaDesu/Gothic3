#version 450

// Skinning happens on the CPU for now, so the vertex data arriving here is
// already posed; the shader only projects it and passes the normal along.

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 lightDirection;
}
push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;

void main()
{
    outNormal = inNormal;
    outTexCoord = inTexCoord;
    gl_Position = push.viewProjection * vec4(inPosition, 1.0);
}
