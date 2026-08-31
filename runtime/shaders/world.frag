#version 450

// No terrain materials yet, so height and slope stand in for them: enough to
// read the shape of the landscape and tell rock from ground.

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 lightDirection;
}
push;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inHeight;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 normal = normalize(inNormal);

    // The world spans roughly -20000..25000 units vertically.
    float height = clamp((inHeight + 20000.0) / 45000.0, 0.0, 1.0);
    vec3 low = vec3(0.24, 0.33, 0.18);   // valley green
    vec3 mid = vec3(0.42, 0.36, 0.24);   // earth
    vec3 high = vec3(0.72, 0.72, 0.74);  // rock and snow
    vec3 ground = height < 0.5 ? mix(low, mid, height * 2.0) : mix(mid, high, (height - 0.5) * 2.0);

    // Steep faces read as bare rock regardless of altitude.
    float slope = 1.0 - clamp(normal.y, 0.0, 1.0);
    ground = mix(ground, vec3(0.38, 0.35, 0.32), smoothstep(0.35, 0.75, slope));

    float key = max(dot(normal, normalize(push.lightDirection.xyz)), 0.0);
    float sky = 0.25 + 0.25 * clamp(normal.y, 0.0, 1.0);

    outColor = vec4(ground * (sky + key * 0.8), 1.0);
}
