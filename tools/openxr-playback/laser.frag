#version 450

layout(push_constant) uniform Beam {
    mat4 view_projection;
    vec4 start, end, eye, color;
} beam;

layout(location = 0) in vec2 coord;
layout(location = 0) out vec4 output_color;

void main()
{
    float alpha;
    if (beam.end.w > 0.5)
        alpha = 1.0 - smoothstep(0.55, 1.0, length(coord));
    else
        alpha = (1.0 - coord.x * coord.x) * mix(1.0, 0.35, coord.y);
    alpha *= beam.color.a;
    /* Premultiplied, like the overlay layer is composited. */
    output_color = vec4(beam.color.rgb * alpha, alpha);
}
