#version 450

/* HEVC Main10 frames from Iris (Q10C = TP10 + UBWC), sampled through the
 * patched Turnip as two 10-bit planes (FMT6_TP10_Y / FMT6_TP10_UV, see
 * tools/mesa-tp10). The planes are padded to whole 192-pixel tiles, so
 * coordinates are scaled by video_scale. Levels and BT.709 are applied as
 * in nv12-projection.frag. */
layout(set = 0, binding = 2) uniform sampler2D luma_plane;
layout(set = 0, binding = 3) uniform sampler2D chroma_plane;

layout(set = 0, binding = 0, std140) uniform ViewUniform {
    mat4 mvp;
    mat4 camera_to_world;
    vec4 fov_tangents;
    ivec4 modes;       /* projection, stereo, eye, swap-eyes */
    vec4 source_info;  /* width, height, inverse width, inverse height */
    vec4 video_scale;  /* visible / padded plane size */
    vec4 levels;       /* black, white, chroma range (8-bit code values) */
    vec4 eye_offset;   /* eye inside the dome (radius 1), dome axes;
                          w: half the fisheye lens angle (rad) */
    vec4 stereo_offset; /* longitude, latitude (rad); flat u, v */
    vec4 picture;      /* brightness, contrast, saturation, gamma */
} view;

layout(location = 0) in vec2 flat_uv;
layout(location = 1) in vec3 projection_direction;
layout(location = 0) out vec4 output_color;

const int PROJECTION_FLAT = 0;
const int PROJECTION_EQUIRECT_180 = 1;
const int PROJECTION_EQUIRECT_360 = 2;
const int PROJECTION_FISHEYE = 3;
const int STEREO_SBS = 1;
const int STEREO_TB = 2;
const float PI = 3.14159265358979323846;

vec2 direction_to_projection_uv(vec3 unnormalized_direction,
                                out bool valid)
{
    vec3 direction = normalize(unnormalized_direction);
    /* The eye may sit off the dome centre (head motion, zoom, height):
     * follow the ray to the unit sphere and look up that point. */
    vec3 eye = view.eye_offset.xyz;
    if (dot(eye, eye) > 0.0) {
        float b = dot(eye, direction);
        float t = -b + sqrt(b * b - dot(eye, eye) + 1.0);
        direction = normalize(eye + t * direction);
    }
    float longitude = atan(direction.x, -direction.z) + view.stereo_offset.x;
    float latitude = asin(clamp(direction.y, -1.0, 1.0)) +
                     view.stereo_offset.y;
    valid = true;

    if (view.modes.x == PROJECTION_FISHEYE) {
        /* Equidistant lens: the angle from the view axis grows linearly
         * with the distance from the circle centre; eye_offset.w is half
         * the lens angle. */
        vec3 d = vec3(cos(latitude) * sin(longitude), sin(latitude),
                      -cos(latitude) * cos(longitude));
        float radius = acos(clamp(-d.z, -1.0, 1.0)) / view.eye_offset.w;
        if (radius > 1.0) {
            valid = false;
            return vec2(0.0);
        }
        float planar = length(d.xy);
        vec2 axis = planar > 1e-6 ? d.xy / planar : vec2(0.0);
        return vec2(0.5 + 0.5 * radius * axis.x,
                    0.5 - 0.5 * radius * axis.y);
    }

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

/* BT.709 with the title's levels; code values on the 8-bit scale. */
vec3 ycbcr_to_rgb(float y, vec2 cbcr)
{
    float luma = (y - view.levels.x) / (view.levels.y - view.levels.x);
    vec2 chroma = (cbcr - 128.0) / view.levels.z;
    vec3 rgb = vec3(luma + 1.5748 * chroma.y,
                    luma - 0.18733 * chroma.x - 0.46813 * chroma.y,
                    luma + 1.8556 * chroma.x);
    return rgb;
}

/* Brightness, contrast, saturation and gamma; neutral values leave the
 * colour untouched. */
vec3 adjust_picture(vec3 rgb)
{
    rgb = (rgb - 0.5) * view.picture.y + 0.5 + view.picture.x;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = clamp(mix(vec3(luma), rgb, view.picture.z), 0.0, 1.0);
    if (view.picture.w != 1.0)
        rgb = pow(rgb, vec3(1.0 / view.picture.w));
    return rgb;
}

void main()
{
    bool valid = true;
    vec2 projection_uv = flat_uv + view.stereo_offset.zw;
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
    vec2 plane_uv = source_uv * view.video_scale.xy;
    float y = texture(luma_plane, plane_uv).r;
    vec2 cbcr = texture(chroma_plane, plane_uv).rg;
    /* Normalized 10-bit codes on the 8-bit scale. */
    vec3 rgb = ycbcr_to_rgb(y * 1023.0 / 4.0, cbcr * 1023.0 / 4.0);
    output_color = vec4(adjust_picture(rgb), 1.0);
}
