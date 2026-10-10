#ifndef STEREONAUT_FORMAT_DETECT_H
#define STEREONAUT_FORMAT_DETECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Projection and stereo layout of a title without stored settings: from
 * tags in the file name ("_180_LR", "3dv"), Jellyfin's Video3DFormat, or the
 * picture itself. Stereo halves look alike with a small horizontal shift;
 * the shape of one eye then tells 180 (1:1), 360 (2:1 with continuous left
 * and right edges) and flat (16:9). Measured on 245 titles of a VR library
 * (trickplay tiles): every tagged title matched its tags. Fisheye cannot be
 * told from a masked 180 picture and is only taken from the name
 * ("fisheye190", "mkx200", "vrca220", "rf52"). */

#define FORMAT_UNKNOWN (-1)

struct format_guess {
    int projection; /* enum nv12_projection_mode or FORMAT_UNKNOWN */
    int stereo;     /* enum nv12_stereo_layout or FORMAT_UNKNOWN */
    int fisheye_fov; /* lens angle in degrees from the name, 0 if unknown */
};

/* Fills the fields the name tags determine; others stay as they are. */
void format_from_name(const char *name, struct format_guess *guess);
/* Jellyfin Video3DFormat ("HalfSideBySide", "FullTopAndBottom", ...):
 * stereo layout only, Jellyfin knows no VR projections. */
void format_from_3d_format(const char *value, struct format_guess *guess);

#define FORMAT_MAX_SAMPLES 32

/* Per-picture measurements; feed pictures from different times (intros are
 * often black or a logo). */
struct format_samples {
    int count;
    float sbs[FORMAT_MAX_SAMPLES];  /* best NCC of left vs right half */
    float tb[FORMAT_MAX_SAMPLES];   /* best NCC of top vs bottom half */
    /* Continuity of the left and right edge of one eye per layout. */
    float wrap[3][FORMAT_MAX_SAMPLES];
};

/* Pictures are 8-bit luma of roughly 160 to 320 pixels width. Flat
 * pictures (black frames) are skipped; returns true if the picture was
 * used. */
bool format_samples_add(struct format_samples *samples, const uint8_t *luma,
                        int width, int height, ptrdiff_t stride);
/* Fills the unknown fields of `guess` from the samples and the video size.
 * Returns true if both fields are known afterwards. */
bool format_samples_decide(const struct format_samples *samples,
                           int video_width, int video_height,
                           struct format_guess *guess);

/* Decodes a JPEG (trickplay tile) to 8-bit luma; *luma is malloc'd. */
int format_decode_jpeg_luma(const unsigned char *data, size_t size,
                            uint8_t **luma, int *width, int *height);

#endif
