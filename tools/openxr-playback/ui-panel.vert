#version 450

/* A UI panel drawn into the eye images: the unit square, placed and sized
 * like the panel's quad layer by the model part of mvp. */
layout(push_constant) uniform Panel {
    mat4 mvp;
} panel;

layout(location = 0) out vec2 uv;

const vec2 positions[4] = vec2[](
    vec2(-0.5, -0.5), vec2(0.5, -0.5),
    vec2(-0.5,  0.5), vec2(0.5,  0.5));

void main()
{
    vec2 position = positions[gl_VertexIndex];
    gl_Position = panel.mvp * vec4(position, 0.0, 1.0);
    /* The image's first row is the panel's top edge. */
    uv = vec2(position.x + 0.5, 0.5 - position.y);
}
