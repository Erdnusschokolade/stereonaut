#ifndef STEREONAUT_XR_INPUT_H
#define STEREONAUT_XR_INPUT_H

#include <stdbool.h>

#include "panel-compositor.h"
#include "xr-session.h"

/* Controller input: the OpenXR actions and their suggested bindings for
 * the controller profiles, and the raw state of trigger, stick, grip and
 * buttons. What the input does is in ui-control.c and input-bindings.c. */

/* Buttons beyond trigger and grip, for the controller bindings. Only
 * valve/frame_controller has them all (paths checked on the Frame with
 * tools/openxr-playback/xr-input-profile-probe.c: the right controller has
 * A/B/X/Y and menu, the left the d-pad and view); the Index profile gets
 * the stick clicks and A/B. */
struct button_input {
    int input;          /* enum binding_input */
    const char *action; /* OpenXR action name */
    const char *label;
    unsigned hand;      /* 0 left, 1 right */
    const char *component;
    bool index;         /* also bound for valve/index_controller */
};

#define BUTTON_INPUT_COUNT 14
extern const struct button_input button_inputs[BUTTON_INPUT_COUNT];

/* Trigger state with hysteresis (button_update). */
struct button {
    bool down;       /* debounced physical state */
    bool suppressed; /* ignore the current press until a genuine release */
};

/* One hand in one frame. */
struct hand_input {
    bool aim_valid;
    XrPosef aim;
    struct panel_hit hit;
    bool select_edge;
    bool select_held;
    XrActionStateBoolean squeeze;
};

/* 1€ filter state of one hand's aim pose (steady_aim). */
struct aim_filter {
    bool valid;
    XrTime time;
    XrPosef pose;
    float speed; /* filtered, rad/s plus m/s */
};

struct xr_input {
    XrActionSet action_set;
    XrAction aim_action;
    XrAction select_action;
    XrAction squeeze_action;
    XrAction stick_action;
    XrAction grip_value_action;
    bool grip_value_down[2];
    float grip_value[2];
    XrAction button_actions[BUTTON_INPUT_COUNT];
    XrAction gaze_action;
    XrSpace gaze_space;
    int stick_direction[2]; /* -1 left, +1 right, 0 centred */
    struct button select[2];
    XrPath hands[2];
    XrSpace aim_spaces[2];
    struct aim_filter aim_filter[2];
    /* Filter cutoff at rest (Hz, 0 = unfiltered) and its rise per speed. */
    float aim_min_cutoff, aim_beta;
};

struct app;

/* Action set, actions, suggested bindings per profile, aim spaces and the
 * eye gaze. */
void initialize_actions(struct app *app);
void read_boolean(struct app *app, XrAction action, XrPath hand,
                  XrActionStateBoolean *state);
/* Trigger with hysteresis: held and the rising edge of a new press. */
void read_select(struct app *app, unsigned hand,
                 struct hand_input *input);
/* Horizontal thumbstick flicks: -1/+1 once per deflection past the
 * threshold (vertical movement is ignored), 0 otherwise. */
int read_stick(struct app *app, unsigned hand);
/* Stick directions of one hand: the dominant direction past the press
 * threshold stays held until it falls under the release threshold. */
void read_stick_directions(struct app *app, unsigned hand,
                           bool down[BINDING_INPUT_COUNT]);
/* Grip per hand for the bindings: the squeeze button or the analog grip
 * past its threshold (with hysteresis). */
void read_grips(struct app *app, const struct hand_input hands[2],
                bool grips[2]);
/* The aim pose of a hand with the raw pose's jitter filtered out: steady
 * while pointing at one spot, without lag on a quick move. `time` is the
 * display time the pose was located for. */
XrPosef steady_aim(struct app *app, unsigned hand, XrPosef aim,
                   XrTime time);
/* Boolean actions: pressed in this sync, or held. */
bool pressed_edge(const XrActionStateBoolean *state);
bool held(const XrActionStateBoolean *state);

#endif
