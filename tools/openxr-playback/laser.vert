#version 450

/* Pointer beam from a controller (start.w: half width in metres) as a band
 * turned towards the eye, or with end.w > 0.5 a dot at its end (radius
 * start.w) facing the eye. */
layout(push_constant) uniform Beam {
    mat4 view_projection;
    vec4 start, end, eye, color;
} beam;

layout(location = 0) out vec2 coord; /* across (-1..1), along (0..1) */

const vec2 corners[4] = vec2[](vec2(-1.0, 0.0), vec2(1.0, 0.0),
                               vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main()
{
    vec2 corner = corners[gl_VertexIndex];
    vec3 position;
    if (beam.end.w > 0.5) {
        vec3 to_eye = normalize(beam.eye.xyz - beam.end.xyz);
        vec3 up = abs(to_eye.y) > 0.99 ? vec3(1.0, 0.0, 0.0)
                                       : vec3(0.0, 1.0, 0.0);
        vec3 right = normalize(cross(up, to_eye));
        up = cross(to_eye, right);
        vec2 disc = vec2(corner.x, corner.y * 2.0 - 1.0);
        position = beam.end.xyz +
                   (right * disc.x + up * disc.y) * beam.start.w;
        coord = disc;
    } else {
        vec3 along = beam.end.xyz - beam.start.xyz;
        vec3 middle = mix(beam.start.xyz, beam.end.xyz, corner.y);
        vec3 side = normalize(cross(along, beam.eye.xyz - middle));
        position = middle + side * corner.x * beam.start.w;
        coord = corner;
    }
    gl_Position = beam.view_projection * vec4(position, 1.0);
}
