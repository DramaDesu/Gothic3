#version 450

// Static geometry: vertices arrive in object space and are placed by a
// per-instance world matrix. The engine stores that matrix row-major with the
// translation in the fourth row, so the rows apply directly.

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 lightDirection;
    float alphaTested; // read by the fragment stage; declared here so the block matches
}
push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inRow0;
layout(location = 4) in vec4 inRow1;
layout(location = 5) in vec4 inRow2;
layout(location = 6) in vec4 inRow3;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out float outHeight;
// The point lights need to know where the surface is, not only which way it faces.
layout(location = 3) out vec3 outWorld;

void main()
{
    vec3 world = inRow0.xyz * inPosition.x + inRow1.xyz * inPosition.y + inRow2.xyz * inPosition.z + inRow3.xyz;

    outNormal = inRow0.xyz * inNormal.x + inRow1.xyz * inNormal.y + inRow2.xyz * inNormal.z;
    outTexCoord = inTexCoord;
    outHeight = world.y;
    outWorld = world;
    gl_Position = push.viewProjection * vec4(world, 1.0);
}
