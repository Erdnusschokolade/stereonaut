#version 450

/* Controller models (XR_EXT_render_model), one draw per glTF node. */
layout(push_constant) uniform Model {
    mat4 mvp;
    vec4 normal_x, normal_y, normal_z; /* node rotation to LOCAL space */
    vec4 color;                        /* base colour factor */
} model;

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec3 world_normal;
layout(location = 1) out vec2 texture_uv;

void main()
{
    gl_Position = model.mvp * vec4(position, 1.0);
    world_normal = mat3(model.normal_x.xyz, model.normal_y.xyz,
                        model.normal_z.xyz) * normal;
    texture_uv = uv;
}
