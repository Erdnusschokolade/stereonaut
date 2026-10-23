#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

#define AUTOFOCUS_TARGET_DEGREES 3.6f /* about 1 m at 63 mm baseline */
#define AUTOFOCUS_BASELINE_METERS 0.063f
#define AUTOFOCUS_MAX_DEGREES 12.0f
#define AUTOFOCUS_MIN_SCORE 0.6f
#define AUTOFOCUS_MIN_VARIANCE 2e-4f
#define GAZE_MARKER_WIDTH 128U
#define GAZE_MARKER_HEIGHT 96U

/* Gaze ray (eye tracking) or, without it, the head's forward ray. */
XrVector3f view_direction(struct app *app, XrPosef head, XrTime time,
                          bool *gaze)
{
    *gaze = false;
    if (app->xr.has_eye_gaze) {
        XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags tracked =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(app->gaze_space, app->xr.space, time,
                                       &location)) &&
            (location.locationFlags & tracked) == tracked) {
            *gaze = true;
            if (!app->autofocus.gaze_logged) {
                printf("eye_gaze tracking=1\n");
                app->autofocus.gaze_logged = true;
            }
            return rotate_vector(location.pose.orientation,
                                 (XrVector3f){0, 0, -1});
        }
    }
    return rotate_vector(head.orientation, (XrVector3f){0, 0, -1});
}

/* Measurement window in both eye pictures around where the viewer looks
 * (equirectangular projections only); false if nothing can be measured. */
bool disparity_window(struct app *app, XrVector3f direction,
                      struct mat4 world_to_dome, XrVector3f eye,
                      float stereo_x, struct disparity_push *push)
{
    if (app->projection_mode == NV12_PROJECTION_FLAT ||
        app->projection_mode == NV12_PROJECTION_FISHEYE ||
        app->stereo_layout == NV12_STEREO_MONO)
        return false;
    XrVector3f d = transform_direction(world_to_dome, direction);
    float length = sqrtf(vector_dot(d, d));
    if (length <= 0.0f)
        return false;
    d.x /= length;
    d.y /= length;
    d.z /= length;
    float b = vector_dot(eye, d);
    float t = -b + sqrtf(b * b - vector_dot(eye, eye) + 1.0f);
    XrVector3f q = {eye.x + t * d.x, eye.y + t * d.y, eye.z + t * d.z};
    const float degrees = PI_F / 180.0f;
    bool full = app->projection_mode == NV12_PROJECTION_EQUIRECT_360;
    float span = full ? 2.0f * PI_F : PI_F;
    /* The left picture is shown moved by half the stereo offset. */
    float longitude = atan2f(q.x, -q.z) + 0.5f * stereo_x * degrees;
    float latitude = asinf(fmaxf(-1.0f, fminf(1.0f, q.y)));
    float stereo_y = app->adjust[PLAYER_ADJUST_STEREO_Y] * 0.5f * degrees;
    if (!full && fabsf(longitude) > 0.5f * PI_F - 3.0f * degrees)
        return false;
    float u = longitude / span + 0.5f;
    if (full)
        u -= floorf(u);
    float v_left = 0.5f - (latitude + stereo_y) / PI_F;
    float v_right = 0.5f - (latitude - stereo_y) / PI_F;
    bool sbs = app->stereo_layout == NV12_STEREO_SBS;
    float u_scale = sbs ? 0.5f : 1.0f, v_scale = sbs ? 1.0f : 0.5f;
    int left_eye = app->swap_eyes ? 1 : 0, right_eye = 1 - left_eye;
    float u_per_degree = degrees / span * u_scale;
    float v_per_degree = degrees / PI_F * v_scale;
    *push = (struct disparity_push){
        .left = {sbs ? 0.5f * left_eye + u * u_scale : u,
                 sbs ? v_left : 0.5f * left_eye + v_left * v_scale,
                 DISPARITY_WINDOW_DEGREES_X / 24.0f * u_per_degree,
                 DISPARITY_WINDOW_DEGREES_Y / 16.0f * v_per_degree},
        .right = {sbs ? 0.5f * right_eye + u * u_scale : u,
                  sbs ? v_right : 0.5f * right_eye + v_right * v_scale,
                  DISPARITY_STEP_DEGREES * u_per_degree,
                  (float)DISPARITY_ZERO},
        .bounds = {sbs ? 0.5f * right_eye : 0.0f,
                   sbs ? 0.5f * right_eye + 0.5f : 1.0f,
                   app->video.coded_width ? (float)app->video.width /
                                          (float)app->video.coded_width : 1.0f,
                   app->video.coded_height ? (float)app->video.height /
                                           (float)app->video.coded_height : 1.0f},
    };
    return true;
}

/* Subject distance of a disparity (degrees). */
static float focus_distance(float disparity)
{
    const float degrees = PI_F / 180.0f;
    return AUTOFOCUS_BASELINE_METERS /
           (2.0f * tanf(0.5f * fmaxf(disparity, 0.05f) * degrees));
}

/* Sets the focus to the subject at `disparity`, or none. */
static void set_focus(struct app *app, bool focused, float disparity)
{
    app->autofocus.focused = focused;
    app->autofocus.focus_disparity = focused ? disparity : 0.0f;
    app->autofocus.distance = focused ? focus_distance(disparity) : 0.0f;
}

/* Reads the last measurement and decides whether to refocus. */
void update_autofocus(struct app *app)
{
    const struct disparity_result *result = app->autofocus.disparity_mapping;
    double now = monotonic_seconds();
    int best = 0;
    for (int i = 1; i < DISPARITY_CANDIDATES; ++i)
        if (result->score[i] > result->score[best])
            best = i;
    float score = result->score[best];
    bool valid = best > 0 && best < DISPARITY_CANDIDATES - 1 &&
                 score >= AUTOFOCUS_MIN_SCORE &&
                 result->left_variance >= AUTOFOCUS_MIN_VARIANCE;
    app->autofocus.valid = valid;
    app->autofocus.score = score;
    app->autofocus.variance = result->left_variance;
    float disparity = 0.0f;
    if (valid) {
        /* Parabola through the peak and its neighbours. */
        float a = result->score[best - 1], c = result->score[best + 1];
        float curvature = a - 2.0f * score + c;
        float refine = curvature < 0.0f ? 0.5f * (a - c) / curvature : 0.0f;
        disparity = ((float)best + refine - (float)DISPARITY_ZERO) *
                    DISPARITY_STEP_DEGREES;
        app->autofocus.disparity = disparity;
        float distance = focus_distance(disparity);
        float limit = app->adjust[PLAYER_ADJUST_FOCUS_DISTANCE];
        float hysteresis = app->adjust[PLAYER_ADJUST_FOCUS_HYSTERESIS];
        if (!app->autofocus.focused) {
            if (!app->focus_threshold || distance <= limit)
                set_focus(app, true, disparity);
        } else if (app->focus_threshold &&
                   distance > limit * (1.0f + hysteresis)) {
            /* Released a little beyond the limit so that a subject right at
             * it does not switch back and forth. */
            set_focus(app, false, 0.0f);
        } else if (fabsf(distance - app->autofocus.distance) >
                   hysteresis * app->autofocus.distance) {
            set_focus(app, true, disparity);
        }
    }
    if (now - app->autofocus.log_time >= 1.0) {
        app->autofocus.log_time = now;
        printf("autofocus gaze=%d valid=%d disparity=%.2f score=%.2f"
               " variance=%.5f focus=%.2f offset=%.2f\n",
               app->autofocus.gaze, valid, disparity, score,
               result->left_variance, app->autofocus.distance,
               app->autofocus.offset);
    }
}

/* Moves the offset towards the current focus, once per frame. A critically
 * damped spring: it starts and ends smoothly, is about 95 % there after
 * the fade time, and a refocus on the way keeps its speed. */
void advance_autofocus(struct app *app)
{
    double now = monotonic_seconds();
    float elapsed = app->autofocus.time > 0.0
                        ? (float)(now - app->autofocus.time)
                        : 0.0f;
    app->autofocus.time = now;
    float strength = app->adjust[PLAYER_ADJUST_AUTOFOCUS];
    if (strength <= 0.0f) {
        app->autofocus.offset = app->autofocus.speed = 0.0f;
        set_focus(app, false, 0.0f);
        return;
    }
    float target = fminf(strength * fmaxf(0.0f,
                                          app->autofocus.focus_disparity -
                                              AUTOFOCUS_TARGET_DEGREES),
                         AUTOFOCUS_MAX_DEGREES);
    float fade = app->adjust[PLAYER_ADJUST_FOCUS_FADE];
    if (fade <= 0.0f) {
        app->autofocus.offset = target;
        app->autofocus.speed = 0.0f;
        return;
    }
    float omega = 4.74f / fade; /* (1 + w t) e^(-w t) = 0.05 at w t = 4.74 */
    float away = app->autofocus.offset - target;
    float push = (app->autofocus.speed + omega * away) * elapsed;
    float decay = expf(-omega * elapsed);
    app->autofocus.offset = target + (away + push) * decay;
    app->autofocus.speed = (app->autofocus.speed - omega * push) * decay;
}

void create_gaze_markers(struct app *app)
{
    static const struct ui_color colors[2] = {
        UI_RGBA(80, 230, 120, 255), UI_RGBA(255, 170, 50, 255),
    };
    for (int i = 0; i < 2; ++i) {
        XrSwapchainCreateInfo info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
            .format = app->panels.ui_format, .sampleCount = 1,
            .width = GAZE_MARKER_WIDTH, .height = GAZE_MARKER_HEIGHT,
            .faceCount = 1, .arraySize = 1, .mipCount = 1,
        };
        need_xr("xrCreateSwapchain(gaze marker)",
                xrCreateSwapchain(app->xr.session, &info,
                                  &app->autofocus.gaze_marker[i]));
        static uint8_t pixels[GAZE_MARKER_WIDTH * GAZE_MARKER_HEIGHT * 4];
        memset(pixels, 0, sizeof(pixels));
        struct ui_canvas canvas = {pixels, GAZE_MARKER_WIDTH,
                                   GAZE_MARKER_HEIGHT, app->panels.ui_bgra};
        ui_stroke_round_rect(&canvas, 3, 3, GAZE_MARKER_WIDTH - 6,
                             GAZE_MARKER_HEIGHT - 6, 8, 4, colors[i]);
        ui_fill_circle(&canvas, GAZE_MARKER_WIDTH / 2.0f,
                       GAZE_MARKER_HEIGHT / 2.0f, 6, UI_RGBA(0, 0, 0, 140));
        ui_fill_circle(&canvas, GAZE_MARKER_WIDTH / 2.0f,
                       GAZE_MARKER_HEIGHT / 2.0f, 4, colors[i]);
        fill_static_swapchain(app, app->autofocus.gaze_marker[i], pixels,
                              GAZE_MARKER_WIDTH, GAZE_MARKER_HEIGHT);
    }
}

XrCompositionLayerQuad make_gaze_marker_layer(const struct app *app)
{
    const float degrees = PI_F / 180.0f;
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->xr.space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->autofocus.gaze_marker[app->autofocus.gaze ? 0 : 1],
            .imageRect = {.extent = {GAZE_MARKER_WIDTH, GAZE_MARKER_HEIGHT}},
        },
        .pose = app->autofocus.gaze_marker_pose,
        .size = {2.0f * GAZE_MARKER_DISTANCE *
                     tanf(0.5f * DISPARITY_WINDOW_DEGREES_X * degrees),
                 2.0f * GAZE_MARKER_DISTANCE *
                     tanf(0.5f * DISPARITY_WINDOW_DEGREES_Y * degrees)},
    };
}

/* Debug line on the panel, refreshed four times a second. */
void update_debug_text(struct app *app)
{
    double now = monotonic_seconds();
    if (!app->gaze_debug || now - app->autofocus.debug_text_time < 0.25)
        return;
    app->autofocus.debug_text_time = now;
    char text[sizeof(app->autofocus.debug_text)];
    const char *source = app->autofocus.gaze ? tr("eye")
                         : app->xr.has_eye_gaze ? tr("head (no gaze)")
                                             : tr("head (no eye tracking)");
    char focus[24];
    snprintf(focus, sizeof(focus), "%s", tr("off"));
    if (app->autofocus.focused)
        snprintf(focus, sizeof(focus), "%.2f m", app->autofocus.distance);
    if (app->projection_mode == NV12_PROJECTION_FLAT ||
        app->projection_mode == NV12_PROJECTION_FISHEYE ||
        app->stereo_layout == NV12_STEREO_MONO)
        snprintf(text, sizeof(text),
                 tr("Gaze: %s · measured only for 180/360 with SBS/TB"),
                 source);
    else
        snprintf(text, sizeof(text),
                 tr("Gaze: %s · disparity %+.2f° · score %.2f · structure "
                    "%.4f · %s · focus %s · offset %.2f°"),
                 source, app->autofocus.disparity, app->autofocus.score,
                 app->autofocus.variance,
                 app->autofocus.valid ? tr("valid") : tr("rejected"), focus,
                 app->autofocus.offset);
    if (strcmp(text, app->autofocus.debug_text) != 0) {
        snprintf(app->autofocus.debug_text, sizeof(app->autofocus.debug_text), "%s", text);
        app->ui_dirty = true;
    }
}

/* The measured key colour (stereo-disparity.comp, sample mode) becomes the
 * title's picked key, with a threshold that suits colour distances. */
void finish_key_pick(struct app *app)
{
    const struct disparity_result *result = app->autofocus.disparity_mapping;
    app->adjust[PLAYER_ADJUST_KEY_Y] = result->score[0];
    app->adjust[PLAYER_ADJUST_KEY_CB] = result->score[1];
    app->adjust[PLAYER_ADJUST_KEY_CR] = result->score[2];
    bool was_picked = app->adjust[PLAYER_ADJUST_KEY_COLOR] > 2.5f;
    app->adjust[PLAYER_ADJUST_KEY_COLOR] = 3.0f;
    if (!was_picked) {
        app->adjust[PLAYER_ADJUST_KEY_THRESHOLD] = 0.08f;
        app->adjust[PLAYER_ADJUST_KEY_SOFTNESS] = 0.04f;
    }
    save_adjust(app, PLAYER_ADJUST_KEY_COLOR);
    app->key_picking = false;
    app->ui_dirty = true;
    printf("key_picked y=%.1f cb=%.1f cr=%.1f\n", result->score[0],
           result->score[1], result->score[2]);
}
