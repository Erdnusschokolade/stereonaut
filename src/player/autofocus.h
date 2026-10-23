#ifndef STEREONAUT_AUTOFOCUS_H
#define STEREONAUT_AUTOFOCUS_H

#include "xr-math.h"
#include "xr-session.h"

/* Autofocus: close subjects in stereo video need strong convergence and
 * easily split into double images. The measurement (stereo-disparity.comp)
 * finds the disparity of the picture where the viewer looks; the eye
 * pictures are then moved apart so that near subjects keep at most the
 * disparity of AUTOFOCUS_TARGET_DEGREES. The focus is set to a subject
 * distance and only changes when the distance changes by more than the
 * hysteresis; with the threshold on, subjects farther than FOCUS_DISTANCE
 * leave the video unchanged. The offset moves to a new focus over the fade
 * time. The same shader samples the key colour for the chroma key picker,
 * and a debug display shows the measurement window along the gaze. */

#define AUTOFOCUS_EVERY_FRAMES 3U

#define DISPARITY_CANDIDATES 128
#define DISPARITY_ZERO 16          /* candidates below zero disparity */
#define DISPARITY_STEP_DEGREES 0.15f
#define DISPARITY_WINDOW_DEGREES_X 4.0f
#define DISPARITY_WINDOW_DEGREES_Y 3.0f

/* Gaze marker: the measurement window (4 x 3 degrees) with a centre dot,
 * this far ahead. */
#define GAZE_MARKER_DISTANCE 1.5f

struct disparity_push {
    float left[4];   /* window centre (texture uv), sample step */
    float right[4];  /* window centre, shift per candidate, zero index */
    float bounds[4]; /* right eye u range, TP10 plane scale */
    float sample_mode[4]; /* x > 0: measure the key colour at left.xy */
};

struct disparity_result {
    float score[DISPARITY_CANDIDATES];
    float left_variance;
};

struct autofocus {
    /* Disparity measurement (NV12 and TP10 variants) and its host-visible
     * result (struct disparity_result). */
    VkPipeline disparity_pipeline, disparity_pipeline_tp10;
    VkBuffer disparity_buffer;
    VkDeviceMemory disparity_memory;
    void *disparity_mapping;
    bool disparity_pending;
    uint64_t disparity_frames;
    double log_time;
    float offset; /* degrees the eye pictures are moved apart */
    float speed;  /* degrees per second of the offset */
    double time;  /* last advance of the offset */
    /* Subject the focus is set to (distance in metres and its disparity
     * in degrees); none: the video is shown unchanged. */
    bool focused;
    float distance, focus_disparity;
    /* Chroma key colour picker: the aim the trigger was pulled with
     * (measured in the next frame), result being read back. */
    bool key_pick_request, key_sample_pending;
    XrPosef key_pick_aim;
    bool gaze;    /* last window followed the eye tracking */
    bool gaze_logged;
    /* Last measurement, for the debug display. */
    bool valid;
    float disparity, score, variance;
    /* Debug display ("Blick anzeigen"): a frame of the measurement window
     * along the gaze (green: eye tracking, orange: head direction) and a
     * text line on the panel. */
    XrSwapchain gaze_marker[2]; /* eye tracking, head direction */
    XrPosef gaze_marker_pose;
    bool gaze_marker_visible;
    double debug_text_time;
    char debug_text[192];
};

struct app;

/* Gaze ray (eye tracking) or, without it, the head's forward ray. */
XrVector3f view_direction(struct app *app, XrPosef head, XrTime time,
                          bool *gaze);
/* Measurement window in both eye pictures around where the viewer looks
 * (equirectangular projections only); false if nothing can be measured. */
bool disparity_window(struct app *app, XrVector3f direction,
                      struct mat4 world_to_dome, XrVector3f eye,
                      float stereo_x, struct disparity_push *push);
/* Reads the last measurement and decides whether to refocus. */
void update_autofocus(struct app *app);
/* Moves the offset towards the current focus, once per frame. */
void advance_autofocus(struct app *app);
/* The measured key colour becomes the title's picked key. */
void finish_key_pick(struct app *app);
void create_gaze_markers(struct app *app);
XrCompositionLayerQuad make_gaze_marker_layer(const struct app *app);
/* Debug line on the panel, refreshed four times a second. */
void update_debug_text(struct app *app);

/* Phases of a frame (render_frame). */
struct frame;
void plan_measurement(struct app *app, struct frame *f);
void collect_measurement(struct app *app);
void record_measurement(struct app *app, struct frame *f);

#endif
