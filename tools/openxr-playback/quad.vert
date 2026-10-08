#version 450

layout(set = 0, binding = 0) uniform ViewUniform {
    mat4 mvp;
} view;

layout(location = 0) out vec2 uv;

const vec2 positions[4] = vec2[](
    vec2(-1.0, -0.5), vec2(1.0, -0.5),
    vec2(-1.0,  0.5), vec2(1.0,  0.5));

void main()
{
    vec2 position = positions[gl_VertexIndex];
    /* A 2:1 plane centred two metres in front of the LOCAL-space origin. */
    gl_Position = view.mvp * vec4(position, -2.0, 1.0);
    uv = vec2(position.x * 0.5 + 0.5, 1.0 - (position.y + 0.5));
}
