#version 450

layout(set = 0, binding = 0) uniform sampler2D panel;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 output_color;

void main()
{
    /* Premultiplied alpha, as for the quad layer. */
    output_color = texture(panel, uv);
}
