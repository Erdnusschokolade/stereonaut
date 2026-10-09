#version 450

/* The immutable VkSampler owns a VkSamplerYcbcrConversion. Sampling performs
 * the NV12 to RGB conversion directly, without an intermediate 8K image. */
layout(set = 0, binding = 1) uniform sampler2D video_frame;

layout(set = 0, binding = 0, std140) uniform ViewUniform {
    mat4 mvp;
    mat4 camera_to_world;
    vec4 fov_tangents;
    ivec4 modes;       /* projection, stereo, eye, swap-eyes */
    vec4 source_info;  /* width, height, inverse width, inverse height */
} view;

layout(location = 0) in vec2 flat_uv;
layout(location = 1) in vec3 projection_direction;
layout(location = 0) out vec4 output_color;

const int PROJECTION_FLAT = 0;
const int PROJECTION_EQUIRECT_180 = 1;
const int PROJECTION_EQUIRECT_360 = 2;
const int STEREO_SBS = 1;
const int STEREO_TB = 2;
const float PI = 3.14159265358979323846;

vec2 direction_to_projection_uv(vec3 unnormalized_direction,
                                out bool valid)
{
    vec3 direction = normalize(unnormalized_direction);
    float longitude = atan(direction.x, -direction.z);
    float latitude = asin(clamp(direction.y, -1.0, 1.0));
    valid = true;

    if (view.modes.x == PROJECTION_EQUIRECT_180) {
        if (abs(longitude) > 0.5 * PI) {
            valid = false;
            return vec2(0.0);
        }
        return vec2(longitude / PI + 0.5,
                    0.5 - latitude / PI);
    }

    /* fract makes the +/-180-degree meridian deterministic. Both source
     * edges are otherwise kept separate to avoid cross-eye filtering. */
    return vec2(fract(longitude / (2.0 * PI) + 0.5),
                0.5 - latitude / PI);
}

vec2 projection_uv_to_stereo_uv(vec2 uv, out vec2 region_min,
                                out vec2 region_max)
{
    int eye = view.modes.z ^ view.modes.w;
    region_min = vec2(0.0);
    region_max = vec2(1.0);
    if (view.modes.y == STEREO_SBS) {
        region_min.x = 0.5 * float(eye);
        region_max.x = region_min.x + 0.5;
        uv.x = region_min.x + 0.5 * uv.x;
    } else if (view.modes.y == STEREO_TB) {
        region_min.y = 0.5 * float(eye);
        region_max.y = region_min.y + 0.5;
        uv.y = region_min.y + 0.5 * uv.y;
    }
    return uv;
}

void main()
{
    bool valid = true;
    vec2 projection_uv = flat_uv;
    if (view.modes.x != PROJECTION_FLAT)
        projection_uv = direction_to_projection_uv(projection_direction,
                                                    valid);
    if (!valid) {
        output_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 region_min, region_max;
    vec2 source_uv = projection_uv_to_stereo_uv(projection_uv,
                                                 region_min, region_max);
    vec2 half_texel = 0.5 * view.source_info.zw;
    source_uv = clamp(source_uv, region_min + half_texel,
                      region_max - half_texel);
    output_color = vec4(texture(video_frame, source_uv).rgb, 1.0);
}
