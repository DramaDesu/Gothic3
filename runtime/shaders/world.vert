#version 450

// Static world geometry: the vertices already sit in world space, so this only
// projects them.

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
layout(location = 2) out float outHeight;

void main()
{
    outNormal = inNormal;
    outTexCoord = inTexCoord;
    outHeight = inPosition.y;
    gl_Position = push.viewProjection * vec4(inPosition, 1.0);
}
