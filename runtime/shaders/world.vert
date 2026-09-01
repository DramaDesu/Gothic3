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

// One colour per vertex per instance. Vertices are shared between instances and
// the lighting is not, so it cannot live in the vertex buffer - the first row's
// unused w carries where this instance's run begins, and -1 means none.
layout(set = 1, binding = 1) readonly buffer Lightmap
{
    uint colours[];
}
lightmap;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out float outHeight;
// The point lights need to know where the surface is, not only which way it faces.
layout(location = 3) out vec3 outWorld;
// The baked light that reached this vertex, and how much sky it can see.
layout(location = 4) out vec3 outBaked;
layout(location = 5) out float outSky;

void main()
{
    vec3 world = inRow0.xyz * inPosition.x + inRow1.xyz * inPosition.y + inRow2.xyz * inPosition.z + inRow3.xyz;

    outNormal = inRow0.xyz * inNormal.x + inRow1.xyz * inNormal.y + inRow2.xyz * inNormal.z;
    outTexCoord = inTexCoord;
    outHeight = world.y;
    outWorld = world;

    outBaked = vec3(0.0);
    outSky = 1.0;
    if (inRow0.w >= 0.0)
    {
        // The low three bytes are the light that reached the vertex; the top
        // byte is not alpha but the fraction of rays that reached the sky.
        const uint packed = lightmap.colours[uint(inRow0.w) + uint(gl_VertexIndex)];
        outBaked = vec3(float((packed >> 16) & 0xFFu), float((packed >> 8) & 0xFFu), float(packed & 0xFFu)) / 255.0;
        outSky = float((packed >> 24) & 0xFFu) / 255.0;
    }
    gl_Position = push.viewProjection * vec4(world, 1.0);
}
