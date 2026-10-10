#define _POSIX_C_SOURCE 200809L
#include "format-detect.h"

#include "../../tools/openxr-playback/nv12-playback-ipc.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

/* ---- file name ---------------------------------------------------------- */

static bool in_list(const char *token, const char *const *list)
{
    for (; *list; ++list)
        if (strcmp(token, *list) == 0)
            return true;
    return false;
}

static int stereo_token(const char *token)
{
    static const char *const sbs[] = {
        "sbs", "lr", "3dh", "hsbs", "fsbs", "halfsbs", "fullsbs",
        "sidebyside", "leftright", NULL,
    };
    static const char *const tb[] = {
        "tb", "bt", "ou", "3dv", "tab", "htab", "ftab", "overunder",
        "topbottom", "topandbottom", NULL,
    };
    static const char *const mono[] = {"mono", "2d", NULL};
    if (in_list(token, sbs))
        return NV12_STEREO_SBS;
    if (in_list(token, tb))
        return NV12_STEREO_TB;
    if (in_list(token, mono))
        return NV12_STEREO_MONO;
    return FORMAT_UNKNOWN;
}

/* "180", "360", optionally "180x180" or "360x180". */
static int projection_prefix(const char *token, size_t *length)
{
    int projection = strncmp(token, "180", 3) == 0
                         ? NV12_PROJECTION_EQUIRECT_180
                     : strncmp(token, "360", 3) == 0
                         ? NV12_PROJECTION_EQUIRECT_360
                         : FORMAT_UNKNOWN;
    *length = 0;
    if (projection == FORMAT_UNKNOWN)
        return projection;
    *length = 3;
    if (strncmp(token + 3, "x180", 4) == 0 ||
        strncmp(token + 3, "x360", 4) == 0)
        *length = 7;
    return projection;
}

static void apply_token(const char *token, struct format_guess *guess)
{
    if (strncmp(token, "vr", 2) == 0 && isdigit((unsigned char)token[2]))
        token += 2; /* "vr180" */
    /* Fisheye lenses: no fisheye projection yet, 180 is the closest. */
    if (strncmp(token, "fisheye", 7) == 0 || strncmp(token, "mkx", 3) == 0 ||
        strncmp(token, "vrca", 4) == 0 || strcmp(token, "rf52") == 0) {
        guess->projection = NV12_PROJECTION_EQUIRECT_180;
        return;
    }
    if (strcmp(token, "flat") == 0 || strcmp(token, "2d") == 0) {
        guess->projection = NV12_PROJECTION_FLAT;
        guess->stereo = NV12_STEREO_MONO;
        return;
    }
    size_t length;
    int projection = projection_prefix(token, &length);
    if (projection != FORMAT_UNKNOWN) {
        int stereo = token[length] ? stereo_token(token + length)
                                   : FORMAT_UNKNOWN;
        if (token[length] && stereo == FORMAT_UNKNOWN)
            return; /* "1800", "360p" */
        guess->projection = projection;
        if (stereo != FORMAT_UNKNOWN)
            guess->stereo = stereo;
        return;
    }
    size_t total = strlen(token);
    if (total > 3) { /* "sbs180", "lr360" */
        char prefix[32];
        if (total - 3 < sizeof(prefix) &&
            projection_prefix(token + total - 3, &length) !=
                FORMAT_UNKNOWN) {
            memcpy(prefix, token, total - 3);
            prefix[total - 3] = '\0';
            int stereo = stereo_token(prefix);
            if (stereo != FORMAT_UNKNOWN) {
                guess->stereo = stereo;
                guess->projection = projection_prefix(token + total - 3,
                                                      &length);
                return;
            }
        }
    }
    int stereo = stereo_token(token);
    if (stereo != FORMAT_UNKNOWN)
        guess->stereo = stereo;
}

void format_from_name(const char *name, struct format_guess *guess)
{
    if (!name)
        return;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    char token[64];
    size_t used = 0;
    for (const char *c = base;; ++c) {
        if (*c && isalnum((unsigned char)*c)) {
            if (used + 1 < sizeof(token))
                token[used++] = (char)tolower((unsigned char)*c);
            continue;
        }
        if (used) {
            token[used] = '\0';
            apply_token(token, guess);
            used = 0;
        }
        if (!*c)
            break;
    }
}

void format_from_3d_format(const char *value, struct format_guess *guess)
{
    if (!value)
        return;
    if (strstr(value, "SideBySide"))
        guess->stereo = NV12_STEREO_SBS;
    else if (strstr(value, "TopAndBottom"))
        guess->stereo = NV12_STEREO_TB;
}

/* ---- picture ------------------------------------------------------------ */

/* Best normalized cross-correlation of two equally sized regions of `pixels`
 * (row stride `stride`), the second shifted horizontally by up to
 * `max_shift` columns either way. */
static float best_ncc(const float *pixels, int stride, int a_x, int a_y,
                      int b_x, int b_y, int width, int height, int max_shift)
{
    int step = max_shift > 16 ? max_shift / 8 : 1;
    float best = -1.0f;
    for (int shift = -max_shift; shift <= max_shift; shift += step) {
        int from = shift > 0 ? shift : 0;
        int to = shift < 0 ? width + shift : width;
        double n = 0, sum_a = 0, sum_b = 0, sum_aa = 0, sum_bb = 0,
               sum_ab = 0;
        for (int y = 0; y < height; ++y) {
            const float *row_a = pixels + (size_t)(a_y + y) * stride + a_x;
            const float *row_b = pixels + (size_t)(b_y + y) * stride + b_x;
            for (int x = from; x < to; ++x) {
                double a = row_a[x], b = row_b[x - shift];
                sum_a += a;
                sum_b += b;
                sum_aa += a * a;
                sum_bb += b * b;
                sum_ab += a * b;
                n += 1;
            }
        }
        if (n < 16)
            continue;
        double variance = (sum_aa - sum_a * sum_a / n) *
                          (sum_bb - sum_b * sum_b / n);
        if (variance <= 1e-9)
            continue;
        float score = (float)((sum_ab - sum_a * sum_b / n) / sqrt(variance));
        if (score > best)
            best = score;
    }
    return best;
}

/* Correlation of the mean of the two leftmost and two rightmost columns of
 * a region: high where a 360 picture continues across its seam. */
static float edge_continuity(const float *pixels, int stride, int x0, int y0,
                             int width, int height)
{
    double n = 0, sum_a = 0, sum_b = 0, sum_aa = 0, sum_bb = 0, sum_ab = 0;
    for (int y = 0; y < height; ++y) {
        const float *row = pixels + (size_t)(y0 + y) * stride + x0;
        double a = 0.5 * (row[0] + row[1]);
        double b = 0.5 * (row[width - 1] + row[width - 2]);
        sum_a += a;
        sum_b += b;
        sum_aa += a * a;
        sum_bb += b * b;
        sum_ab += a * b;
        n += 1;
    }
    double variance = (sum_aa - sum_a * sum_a / n) *
                      (sum_bb - sum_b * sum_b / n);
    if (variance <= 1e-9)
        return 0.0f;
    return (float)((sum_ab - sum_a * sum_b / n) / sqrt(variance));
}

bool format_samples_add(struct format_samples *samples, const uint8_t *luma,
                        int width, int height, ptrdiff_t stride)
{
    if (samples->count >= FORMAT_MAX_SAMPLES || width < 32 || height < 32)
        return false;
    float *pixels = malloc((size_t)width * (size_t)height * sizeof(*pixels));
    if (!pixels)
        return false;
    double sum = 0, sum_squares = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float value = luma[(size_t)y * (size_t)stride + (size_t)x] /
                          255.0f;
            pixels[(size_t)y * (size_t)width + (size_t)x] = value;
            sum += value;
            sum_squares += (double)value * value;
        }
    double n = (double)width * height;
    double deviation = sqrt(fmax(0.0, sum_squares / n - (sum / n) *
                                                          (sum / n)));
    if (deviation < 0.04) { /* black frame, fade or title card */
        free(pixels);
        return false;
    }
    int half_w = width / 2, half_h = height / 2;
    int i = samples->count++;
    /* Near subjects shift the halves by up to a tenth of an eye's width. */
    samples->sbs[i] = best_ncc(pixels, width, 0, 0, half_w, 0, half_w,
                               height, half_w / 5 > 1 ? half_w / 5 : 1);
    samples->tb[i] = best_ncc(pixels, width, 0, 0, 0, half_h, width, half_h,
                              width / 12 > 1 ? width / 12 : 1);
    samples->wrap[NV12_STEREO_MONO][i] =
        edge_continuity(pixels, width, 0, 0, width, height);
    samples->wrap[NV12_STEREO_SBS][i] =
        edge_continuity(pixels, width, 0, 0, half_w, height);
    samples->wrap[NV12_STEREO_TB][i] =
        edge_continuity(pixels, width, 0, 0, width, half_h);
    free(pixels);
    return true;
}

static int compare_floats(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float median(const float *values, int count)
{
    float sorted[FORMAT_MAX_SAMPLES];
    memcpy(sorted, values, (size_t)count * sizeof(*sorted));
    qsort(sorted, (size_t)count, sizeof(*sorted), compare_floats);
    return count % 2 ? sorted[count / 2]
                     : 0.5f * (sorted[count / 2 - 1] + sorted[count / 2]);
}

bool format_samples_decide(const struct format_samples *samples,
                           int video_width, int video_height,
                           struct format_guess *guess)
{
    if (samples->count >= 4 && guess->stereo == FORMAT_UNKNOWN) {
        float sbs = median(samples->sbs, samples->count);
        float tb = median(samples->tb, samples->count);
        if (sbs > 0.6f && sbs > tb + 0.1f)
            guess->stereo = NV12_STEREO_SBS;
        else if (tb > 0.6f && tb > sbs + 0.1f)
            guess->stereo = NV12_STEREO_TB;
        else if (sbs < 0.45f && tb < 0.45f)
            guess->stereo = NV12_STEREO_MONO;
    }
    if (guess->projection == FORMAT_UNKNOWN &&
        guess->stereo != FORMAT_UNKNOWN && video_width > 0 &&
        video_height > 0) {
        double eye_w = video_width, eye_h = video_height;
        if (guess->stereo == NV12_STEREO_SBS)
            eye_w /= 2;
        else if (guess->stereo == NV12_STEREO_TB)
            eye_h /= 2;
        double aspect = eye_w / eye_h;
        if (aspect >= 0.85 && aspect <= 1.15) {
            /* Also 16:9 side by side with letterboxed square eyes. */
            guess->projection = NV12_PROJECTION_EQUIRECT_180;
        } else if (aspect >= 1.85 && aspect <= 2.15) {
            if (samples->count >= 4 &&
                median(samples->wrap[guess->stereo], samples->count) > 0.8f)
                guess->projection = NV12_PROJECTION_EQUIRECT_360;
        } else if (aspect >= 1.6 && aspect < 1.85) {
            guess->projection = NV12_PROJECTION_FLAT;
        }
    }
    return guess->projection != FORMAT_UNKNOWN &&
           guess->stereo != FORMAT_UNKNOWN;
}

/* ---- JPEG --------------------------------------------------------------- */

struct jpeg_failure {
    struct jpeg_error_mgr manager;
    jmp_buf jump;
};

static void jpeg_fail(j_common_ptr info)
{
    longjmp(((struct jpeg_failure *)info->err)->jump, 1);
}

int format_decode_jpeg_luma(const unsigned char *data, size_t size,
                            uint8_t **luma, int *width, int *height)
{
    struct jpeg_decompress_struct info;
    struct jpeg_failure failure;
    uint8_t *volatile pixels = NULL;
    *luma = NULL;
    info.err = jpeg_std_error(&failure.manager);
    failure.manager.error_exit = jpeg_fail;
    if (setjmp(failure.jump)) {
        jpeg_destroy_decompress(&info);
        free(pixels);
        return -1;
    }
    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, data, (unsigned long)size);
    jpeg_read_header(&info, TRUE);
    info.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&info);
    pixels = malloc((size_t)info.output_width * info.output_height);
    if (!pixels) {
        jpeg_destroy_decompress(&info);
        return -1;
    }
    while (info.output_scanline < info.output_height) {
        JSAMPROW row = pixels + (size_t)info.output_scanline *
                                    info.output_width;
        jpeg_read_scanlines(&info, &row, 1);
    }
    *width = (int)info.output_width;
    *height = (int)info.output_height;
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    *luma = pixels;
    return 0;
}
