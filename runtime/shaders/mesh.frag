#version 450

// No textures yet: a two-light setup over a neutral albedo, which is enough to
// read the silhouette and see that normals survived skinning.

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 lightDirection;
}
push;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 normal = normalize(inNormal);
    float key = max(dot(normal, normalize(push.lightDirection.xyz)), 0.0);
    float fill = max(dot(normal, normalize(vec3(-0.4, 0.2, -0.8))), 0.0) * 0.35;
    float ambient = 0.18;

    vec3 albedo = vec3(0.62, 0.60, 0.55);
    outColor = vec4(albedo * (ambient + key + fill), 1.0);
}
