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
layout(location = 4) in vec3 inBaked;
layout(location = 5) in float inSky;
layout(location = 6) in vec3 inIncident;
layout(location = 7) in vec2 inPatch;

// Every baked patch in the scene, packed into one image.
layout(set = 1, binding = 4) uniform sampler2D lightmapAtlas;

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

    // How the baked light and the daylight go together. The fourth component of
    // the light direction chooses: zero adds the patch to the daylight, one lets
    // it stand in for it. A surface the bake covered already accounts for the
    // sun that reached it, so adding both counts the same light twice - which is
    // what made the room brighter and flatter at once.
    float replace = push.lightDirection.w;

    float day = 0.55 + 0.45 * max(dot(normal, normalize(push.lightDirection.xyz)), 0.0);
    vec3 lit = sampled.rgb * day * inSky;

    // The light the bake recorded at this vertex, answering to where it came
    // from: a wall facing the window takes it and the wall beside it does not.
    // Half the term is left unshaped, because the bake stores one dominant
    // direction and a surface still receives light from the rest of the room.
    float facing = dot(inIncident, inIncident) > 0.01 ? max(dot(normal, normalize(inIncident)), 0.0) : 1.0;
    vec3 baked = inBaked * (0.5 + 0.5 * facing);

    if (inPatch.x >= 0.0)
    {
        // A patch of light on this surface, at the texel the chart says.
        baked += texture(lightmapAtlas, inPatch).rgb;
        lit = mix(lit, sampled.rgb * 0.25 * inSky, replace);
    }

    lit += sampled.rgb * baked;

    // The game's own tone curve, from its shipped ip_hdri.fx and the defaults in
    // ge3.INI: Render.HDRExposure 2.85 and Render.HDRGamma 0.60. Without it a
    // texel of 0.15 stays 0.15 and the world reads as night; through it the same
    // texel lands at 0.53. The darkness was never in the textures.
    vec3 mapped = 1.0 - exp(-2.85 * lit);
    mapped = pow(max(mapped, vec3(1e-5)), vec3(0.60));
    outColor = vec4(mapped, 1.0);
}
