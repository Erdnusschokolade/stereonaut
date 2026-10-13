#version 450

layout(set = 0, binding = 0) uniform sampler2D base_color;

layout(push_constant) uniform Model {
    mat4 mvp;
    vec4 normal_x, normal_y, normal_z;
    vec4 color;
} model;

layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec2 texture_uv;
layout(location = 0) out vec4 output_color;

void main()
{
    vec3 base = texture(base_color, texture_uv).rgb * model.color.rgb;
    /* Soft light from above in front, so the dark plastic keeps its
     * shape. */
    vec3 light = normalize(vec3(0.3, 1.0, 0.6));
    float diffuse = max(dot(normalize(world_normal), light), 0.0);
    output_color = vec4(base * (0.45 + 0.75 * diffuse), 1.0);
}
