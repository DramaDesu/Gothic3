#version 450

// No terrain materials yet, so height and slope stand in for them: enough to
// read the shape of the landscape and tell rock from ground.

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 lightDirection;
    float alphaTested;
}
push;

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inHeight;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 normal = normalize(inNormal);
    vec4 sampled = texture(diffuseMap, inTexCoord);
    // Foliage is drawn as quads whose texture is mostly empty, so its
    // transparent pixels are thrown away rather than blended. Solid surfaces are
    // not tested: a wall whose texture carries anything but 1 in its alpha
    // channel would come out riddled with holes.
    if (push.alphaTested > 0.5 && sampled.a < 0.5)
        discard;
    vec3 ground = sampled.rgb;

    // The diffuse maps already carry baked shading, so the lighting here only
    // has to give the relief some direction without crushing the forests.
    float key = max(dot(normal, normalize(push.lightDirection.xyz)), 0.0);
    float sky = 0.75 + 0.25 * clamp(normal.y, 0.0, 1.0);

    outColor = vec4(ground * (sky + key * 0.45), 1.0);
}
