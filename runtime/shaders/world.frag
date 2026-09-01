#version 450

// The world's own lighting. The diffuse maps carry baked shading of their own,
// which is why a strong light here turns the forests black - so daylight is
// gentle and the point lights add to it rather than replacing it.

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 lightDirection;
    float alphaTested;
}
push;

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;

// The lights nearest the camera, chosen per frame. count sits in x; the rest of
// the first vector is spare.
layout(set = 1, binding = 0) uniform Lights
{
    vec4 count;
    vec4 positionRange[16];
    vec4 colour[16];
}
lights;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inHeight;
layout(location = 3) in vec3 inWorld;

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

    float day = 0.55 + 0.45 * max(dot(normal, normalize(push.lightDirection.xyz)), 0.0);
    vec3 lit = sampled.rgb * day;

    int used = int(lights.count.x);
    for (int index = 0; index < used; ++index)
    {
        vec3 toLight = lights.positionRange[index].xyz - inWorld;
        float range = lights.positionRange[index].w;
        float distance = length(toLight);
        if (distance >= range)
            continue;

        // Falls off with the square of distance and is cut to nothing at the
        // range the entity declares, so a lamp lights its room and not the hill.
        float fade = 1.0 - distance / range;
        float attenuation = fade * fade;
        float facing = max(dot(normal, toLight / max(distance, 0.001)), 0.0);
        lit += sampled.rgb * lights.colour[index].rgb * (facing * attenuation);
    }

    outColor = vec4(lit, 1.0);
}
