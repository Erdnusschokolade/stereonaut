#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 output_color;

void main()
{
    /* Deliberately asymmetric marker pattern for orientation validation. */
    vec3 top = vec3(0.03, 0.26, 0.65);
    vec3 bottom = vec3(0.58, 0.07, 0.03);
    vec3 color = mix(bottom, top, uv.y);
    vec2 cells = floor(uv * vec2(16.0, 8.0));
    if (mod(cells.x + cells.y, 2.0) < 1.0)
        color *= 0.68;
    if (uv.x < 0.08 && uv.y > 0.82)
        color = vec3(0.95, 0.95, 0.10);
    output_color = vec4(color, 1.0);
}
