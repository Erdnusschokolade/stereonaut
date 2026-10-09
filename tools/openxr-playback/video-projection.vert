#version 450

layout(set = 0, binding = 0, std140) uniform ViewUniform {
    mat4 mvp;
    mat4 camera_to_world;
    vec4 fov_tangents; /* left, right, down, up */
    ivec4 modes;       /* projection, stereo, eye, swap-eyes */
    vec4 source_info;  /* width, height, inverse width, inverse height */
} view;

layout(location = 0) out vec2 flat_uv;
layout(location = 1) out vec3 projection_direction;

const int PROJECTION_FLAT = 0;

const vec2 positions[4] = vec2[](
    vec2(-1.0, -0.5), vec2(1.0, -0.5),
    vec2(-1.0,  0.5), vec2(1.0,  0.5));

void main()
{
    vec2 position = positions[gl_VertexIndex];
    vec2 view_ndc = vec2(position.x, position.y * 2.0);

    if (view.modes.x == PROJECTION_FLAT) {
        /* A 2:1 plane centred two metres in front of LOCAL-space origin. */
        gl_Position = view.mvp * vec4(position, -2.0, 1.0);
    } else {
        /* Positive Vulkan viewport height maps NDC -Y to framebuffer top. */
        gl_Position = vec4(view_ndc.x, -view_ndc.y, 0.0, 1.0);
    }

    flat_uv = vec2(position.x * 0.5 + 0.5,
                   1.0 - (position.y + 0.5));
    /* Keep the corner rays unnormalized while the rasterizer interpolates
     * them. Normalizing each corner first bends the reconstructed ray field,
     * and asymmetric per-eye FOVs then produce different horizontal and
     * vertical image offsets. The fragment shader normalizes the final ray. */
    vec3 view_direction = vec3(
        mix(view.fov_tangents.x, view.fov_tangents.y,
            view_ndc.x * 0.5 + 0.5),
        mix(view.fov_tangents.z, view.fov_tangents.w,
            view_ndc.y * 0.5 + 0.5),
        -1.0);
    projection_direction = mat3(view.camera_to_world) * view_direction;
}
