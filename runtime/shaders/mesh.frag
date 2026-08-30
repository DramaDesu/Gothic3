#version 450

// Gothic 3 normal maps are DXT5nm-swizzled: the red and blue channels are empty,
// Y lives in green and X in alpha, so Z has to be rebuilt here.

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 lightDirection;
}
push;

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;
layout(set = 0, binding = 1) uniform sampler2D normalMap;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 albedo = texture(diffuseMap, inTexCoord);

    vec4 packed = texture(normalMap, inTexCoord);
    vec2 xy = vec2(packed.a, packed.g) * 2.0 - 1.0;
    float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
    vec3 detail = vec3(xy, z);

    // Without tangents plumbed through, the detail normal is blended into the
    // geometric one: enough to show the map is being read correctly.
    vec3 normal = normalize(normalize(inNormal) + detail * 0.35);

    float key = max(dot(normal, normalize(push.lightDirection.xyz)), 0.0);
    float fill = max(dot(normal, normalize(vec3(-0.4, 0.2, -0.8))), 0.0) * 0.35;
    float ambient = 0.20;

    outColor = vec4(albedo.rgb * (ambient + key + fill), 1.0);
}
