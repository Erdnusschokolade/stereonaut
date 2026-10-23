#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

/* The select action is analog so the press point can be lower than the
 * trigger's full click travel. */
#define SELECT_PRESS_THRESHOLD 0.50f
#define SELECT_RELEASE_THRESHOLD 0.03f
/* Thumbstick left/right: same low threshold as the trigger, one action per
 * deflection. */
#define STICK_PRESS_THRESHOLD 0.30f
#define STICK_RELEASE_THRESHOLD 0.15f
/* Grip as a modifier of button bindings: its analog value, because the
 * Frame's grip click needs a firm squeeze that a hand also moving the stick
 * rarely keeps up. */
#define GRIP_PRESS_THRESHOLD 0.40f
#define GRIP_RELEASE_THRESHOLD 0.25f

const struct button_input button_inputs[] = {
    {BINDING_LEFT_STICK_CLICK, "left_stick_click", "Stick-Klick links", 0,
     "thumbstick/click", true},
    {BINDING_RIGHT_STICK_CLICK, "right_stick_click", "Stick-Klick rechts",
     1, "thumbstick/click", true},
    {BINDING_LEFT_BUMPER, "left_bumper", "Bumper links", 0, "bumper/click",
     false},
    {BINDING_RIGHT_BUMPER, "right_bumper", "Bumper rechts", 1,
     "bumper/click", false},
    {BINDING_DPAD_UP, "dpad_up", "Kreuz hoch", 0, "dpad_up/click", false},
    {BINDING_DPAD_DOWN, "dpad_down", "Kreuz runter", 0, "dpad_down/click",
     false},
    {BINDING_DPAD_LEFT, "dpad_left", "Kreuz links", 0, "dpad_left/click",
     false},
    {BINDING_DPAD_RIGHT, "dpad_right", "Kreuz rechts", 0, "dpad_right/click",
     false},
    {BINDING_VIEW, "view", "View", 0, "view/click", false},
    {BINDING_A, "button_a", "A", 1, "a/click", true},
    {BINDING_B, "button_b", "B", 1, "b/click", true},
    {BINDING_X, "button_x", "X", 1, "x/click", false},
    {BINDING_Y, "button_y", "Y", 1, "y/click", false},
    {BINDING_MENU, "menu", "Menue", 1, "menu/click", false},
};
_Static_assert(ARRAY_COUNT(button_inputs) == BUTTON_INPUT_COUNT,
               "BUTTON_INPUT_COUNT differs from button_inputs");

static XrPath xr_path(struct app *app, const char *text)
{
    XrPath path = XR_NULL_PATH;
    need_xr("xrStringToPath", xrStringToPath(app->xr.instance, text, &path));
    return path;
}

static void create_action(XrActionSet set, XrActionType type,
                          const char *name, const char *localized,
                          XrPath hands[2], XrAction *action)
{
    XrActionCreateInfo info = {
        .type = XR_TYPE_ACTION_CREATE_INFO,
        .actionType = type,
        .countSubactionPaths = 2,
        .subactionPaths = hands,
    };
    snprintf(info.actionName, sizeof(info.actionName), "%s", name);
    snprintf(info.localizedActionName, sizeof(info.localizedActionName),
             "%s", localized);
    need_xr("xrCreateAction", xrCreateAction(set, &info, action));
}

void initialize_actions(struct app *app)
{
    app->input.hands[0] = xr_path(app, "/user/hand/left");
    app->input.hands[1] = xr_path(app, "/user/hand/right");
    XrActionSetCreateInfo set = {
        .type = XR_TYPE_ACTION_SET_CREATE_INFO,
    };
    snprintf(set.actionSetName, sizeof(set.actionSetName), "player");
    snprintf(set.localizedActionSetName, sizeof(set.localizedActionSetName),
             "Stereonaut Player");
    need_xr("xrCreateActionSet",
            xrCreateActionSet(app->xr.instance, &set, &app->input.action_set));
    create_action(app->input.action_set, XR_ACTION_TYPE_POSE_INPUT,
                  "aim", "Zeigen", app->input.hands, &app->input.aim_action);
    create_action(app->input.action_set, XR_ACTION_TYPE_FLOAT_INPUT,
                  "select", "Auswaehlen", app->input.hands,
                  &app->input.select_action);
    create_action(app->input.action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,
                  "squeeze", "Greifen", app->input.hands,
                  &app->input.squeeze_action);
    create_action(app->input.action_set, XR_ACTION_TYPE_VECTOR2F_INPUT,
                  "stick", "Blaettern und Spulen", app->input.hands,
                  &app->input.stick_action);
    create_action(app->input.action_set, XR_ACTION_TYPE_FLOAT_INPUT,
                  "grip_value", "Griff (Kombinationen)", app->input.hands,
                  &app->input.grip_value_action);
    for (size_t i = 0; i < ARRAY_COUNT(button_inputs); ++i)
        create_action(app->input.action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,
                      button_inputs[i].action, button_inputs[i].label,
                      app->input.hands, &app->input.button_actions[i]);
    struct {
        const char *profile;
        const char *select_component;
        const char *squeeze_component;
        bool thumbstick;
        bool enabled;
        int buttons; /* button_inputs: 0 none, 1 Index subset, 2 all */
        bool grip_value; /* has squeeze/value */
    } profiles[] = {
        {"/interaction_profiles/valve/frame_controller", "trigger/value",
         "squeeze/click", true, app->xr.has_valve_frame_controller, 2, true},
        {"/interaction_profiles/khr/generic_controller", "trigger/value",
         "squeeze/value", true, app->xr.has_generic_controller, 0, true},
        {"/interaction_profiles/valve/index_controller", "trigger/value",
         "squeeze/value", true, true, 1, true},
        /* The common PCVR profiles use the same core affordances as Index:
         * trigger for pointing, squeeze for panel grab and a thumbstick for
         * seek/volume. Keep face-button suggestions to Index: their names
         * and hands differ between Touch, WMR and Vive. */
        {"/interaction_profiles/oculus/touch_controller", "trigger/value",
         "squeeze/value", true, app->xr.has_pcvr_controller, 0, true},
        {"/interaction_profiles/microsoft/motion_controller", "trigger/value",
         "squeeze/value", true, app->xr.has_pcvr_controller, 0, true},
        {"/interaction_profiles/htc/vive_cosmos_controller", "trigger/value",
         "squeeze/click", true, app->xr.has_pcvr_controller, 0, false},
        /* Original Vive wands expose a trackpad rather than a thumbstick;
         * it is still a Vector2f input and maps naturally to navigation. */
        {"/interaction_profiles/htc/vive_controller", "trigger/value",
         "squeeze/click", true, app->xr.has_pcvr_controller, 0, false},
        /* No squeeze button or stick: panel grab, show/hide, page flips and
         * stick seeking are unavailable. */
        {"/interaction_profiles/khr/simple_controller", "select/click", NULL,
         false, true, 0, false},
    };
    static const char *const hand_names[2] = {"left", "right"};
    unsigned accepted = 0;
    for (size_t p = 0; p < ARRAY_COUNT(profiles); ++p) {
        if (!profiles[p].enabled)
            continue;
        XrActionSuggestedBinding bindings[10 + ARRAY_COUNT(button_inputs)];
        uint32_t count = 0;
        char text[4][2][96];
        for (unsigned hand = 0; hand < 2; ++hand) {
            snprintf(text[0][hand], sizeof(text[0][hand]),
                     "/user/hand/%s/input/aim/pose", hand_names[hand]);
            bindings[count++] = (XrActionSuggestedBinding){
                app->input.aim_action, xr_path(app, text[0][hand])};
            snprintf(text[1][hand], sizeof(text[1][hand]),
                     "/user/hand/%s/input/%s", hand_names[hand],
                     profiles[p].select_component);
            bindings[count++] = (XrActionSuggestedBinding){
                app->input.select_action, xr_path(app, text[1][hand])};
            if (profiles[p].squeeze_component) {
                snprintf(text[2][hand], sizeof(text[2][hand]),
                         "/user/hand/%s/input/%s", hand_names[hand],
                         profiles[p].squeeze_component);
                bindings[count++] = (XrActionSuggestedBinding){
                    app->input.squeeze_action, xr_path(app, text[2][hand])};
            }
            if (profiles[p].thumbstick) {
                const char *stick = strstr(profiles[p].profile,
                                           "vive_controller")
                                        ? "trackpad" : "thumbstick";
                snprintf(text[3][hand], sizeof(text[3][hand]),
                         "/user/hand/%s/input/%s", hand_names[hand], stick);
                bindings[count++] = (XrActionSuggestedBinding){
                    app->input.stick_action, xr_path(app, text[3][hand])};
            }
            if (profiles[p].grip_value) {
                char path[96];
                snprintf(path, sizeof(path),
                         "/user/hand/%s/input/squeeze/value",
                         hand_names[hand]);
                bindings[count++] = (XrActionSuggestedBinding){
                    app->input.grip_value_action, xr_path(app, path)};
            }
        }
        for (size_t i = 0; i < ARRAY_COUNT(button_inputs); ++i) {
            if (profiles[p].buttons == 0 ||
                (profiles[p].buttons == 1 && !button_inputs[i].index))
                continue;
            char path[96];
            snprintf(path, sizeof(path), "/user/hand/%s/input/%s",
                     hand_names[button_inputs[i].hand],
                     button_inputs[i].component);
            bindings[count++] = (XrActionSuggestedBinding){
                app->input.button_actions[i], xr_path(app, path)};
        }
        XrInteractionProfileSuggestedBinding suggestion = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = xr_path(app, profiles[p].profile),
            .countSuggestedBindings = count,
            .suggestedBindings = bindings,
        };
        XrResult result =
            xrSuggestInteractionProfileBindings(app->xr.instance, &suggestion);
        printf("controller_bindings profile=%s result=%d\n",
               profiles[p].profile, result);
        if (XR_SUCCEEDED(result))
            ++accepted;
    }
    need(accepted > 0, "no controller interaction profile was accepted");
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo space = {
            .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
            .action = app->input.aim_action,
            .subactionPath = app->input.hands[hand],
            .poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}},
        };
        need_xr("xrCreateActionSpace",
                xrCreateActionSpace(app->xr.session, &space,
                                    &app->input.aim_spaces[hand]));
    }
    if (app->xr.has_eye_gaze) {
        XrActionCreateInfo gaze = {
            .type = XR_TYPE_ACTION_CREATE_INFO,
            .actionType = XR_ACTION_TYPE_POSE_INPUT,
        };
        snprintf(gaze.actionName, sizeof(gaze.actionName), "gaze");
        snprintf(gaze.localizedActionName,
                 sizeof(gaze.localizedActionName), "Blickrichtung");
        need_xr("xrCreateAction(gaze)",
                xrCreateAction(app->input.action_set, &gaze,
                               &app->input.gaze_action));
        XrActionSuggestedBinding binding = {
            app->input.gaze_action,
            xr_path(app, "/user/eyes_ext/input/gaze_ext/pose"),
        };
        XrInteractionProfileSuggestedBinding suggestion = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile =
                xr_path(app, "/interaction_profiles/ext/eye_gaze_interaction"),
            .countSuggestedBindings = 1,
            .suggestedBindings = &binding,
        };
        XrResult result =
            xrSuggestInteractionProfileBindings(app->xr.instance, &suggestion);
        printf("eye_gaze_binding result=%d\n", result);
        XrActionSpaceCreateInfo space = {
            .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
            .action = app->input.gaze_action,
            .poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}},
        };
        if (XR_FAILED(result) ||
            XR_FAILED(xrCreateActionSpace(app->xr.session, &space,
                                          &app->input.gaze_space)))
            app->xr.has_eye_gaze = false;
    }
    XrSessionActionSetsAttachInfo attach = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &app->input.action_set,
    };
    need_xr("xrAttachSessionActionSets",
            xrAttachSessionActionSets(app->xr.session, &attach));
    app->overlay.render_models_dirty = true;
}

void read_boolean(struct app *app, XrAction action, XrPath hand,
                  XrActionStateBoolean *state)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = action,
        .subactionPath = hand,
    };
    *state = (XrActionStateBoolean){.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    need_xr("xrGetActionStateBoolean",
            xrGetActionStateBoolean(app->xr.session, &info, state));
}

/* Horizontal thumbstick flicks: -1/+1 once per deflection past the
 * threshold (vertical movement is ignored), 0 otherwise. */
int read_stick(struct app *app, unsigned hand)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = app->input.stick_action,
        .subactionPath = app->input.hands[hand],
    };
    XrActionStateVector2f state = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    need_xr("xrGetActionStateVector2f",
            xrGetActionStateVector2f(app->xr.session, &info, &state));
    float x = state.isActive ? state.currentState.x : 0.0f;
    float y = state.isActive ? state.currentState.y : 0.0f;
    int direction = app->input.stick_direction[hand];
    if (direction && direction * x < STICK_RELEASE_THRESHOLD) {
        app->input.stick_direction[hand] = 0;
        return 0;
    }
    if (!direction) {
        if (fabsf(x) < STICK_PRESS_THRESHOLD || fabsf(y) > fabsf(x))
            return 0;
        direction = x > 0.0f ? 1 : -1;
        app->input.stick_direction[hand] = direction;
        return direction;
    }
    return 0;
}

/* One frame of an analog button; true on the rising edge of a new press.
 * While the action is inactive (focus loss, SteamVR dashboard) the last
 * state is kept: treating that as a release turned a trigger still held on
 * return into a phantom press and lifted the suppression after "back". */
static bool button_update(struct button *button, bool active, float value,
                          bool *held)
{
    bool was = button->down;
    if (active) {
        if (!button->down && value >= SELECT_PRESS_THRESHOLD)
            button->down = true;
        else if (button->down && value < SELECT_RELEASE_THRESHOLD)
            button->down = false;
        if (!button->down)
            button->suppressed = false;
    }
    *held = button->down;
    return active && button->down && !was && !button->suppressed;
}

void read_select(struct app *app, unsigned hand,
                 struct hand_input *input)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = app->input.select_action,
        .subactionPath = app->input.hands[hand],
    };
    XrActionStateFloat state = {.type = XR_TYPE_ACTION_STATE_FLOAT};
    need_xr("xrGetActionStateFloat",
            xrGetActionStateFloat(app->xr.session, &info, &state));
    input->select_edge =
        button_update(&app->input.select[hand], state.isActive,
                      state.currentState, &input->select_held);
}

bool pressed_edge(const XrActionStateBoolean *state)
{
    return state->isActive && state->changedSinceLastSync &&
           state->currentState;
}

bool held(const XrActionStateBoolean *state)
{
    return state->isActive && state->currentState;
}

/* Stick directions of one hand: the dominant direction past the press
 * threshold stays held until it falls under the release threshold. */
void read_stick_directions(struct app *app, unsigned hand,
                           bool down[BINDING_INPUT_COUNT])
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = app->input.stick_action,
        .subactionPath = app->input.hands[hand],
    };
    XrActionStateVector2f state = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    need_xr("xrGetActionStateVector2f",
            xrGetActionStateVector2f(app->xr.session, &info, &state));
    float x = state.isActive ? state.currentState.x : 0.0f;
    float y = state.isActive ? state.currentState.y : 0.0f;
    const float values[4] = {-x, x, y, -y}; /* left, right, up, down */
    for (int d = 0; d < 4; ++d)
        if (app->controls.held[binding_stick(hand, d)] &&
            values[d] >= STICK_RELEASE_THRESHOLD) {
            down[binding_stick(hand, d)] = true;
            return;
        }
    int best = -1;
    float best_value = STICK_PRESS_THRESHOLD;
    for (int d = 0; d < 4; ++d)
        if (values[d] >= best_value) {
            best = d;
            best_value = values[d];
        }
    if (best >= 0)
        down[binding_stick(hand, best)] = true;
}

/* Grip per hand for the bindings: the squeeze button or the analog grip
 * past its threshold (with hysteresis). */
void read_grips(struct app *app, const struct hand_input hands[2],
                bool grips[2])
{
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->input.grip_value_action,
            .subactionPath = app->input.hands[hand],
        };
        XrActionStateFloat state = {.type = XR_TYPE_ACTION_STATE_FLOAT};
        need_xr("xrGetActionStateFloat(grip)",
                xrGetActionStateFloat(app->xr.session, &info, &state));
        float value = state.isActive ? state.currentState : 0.0f;
        app->input.grip_value[hand] = value;
        app->input.grip_value_down[hand] =
            value >= (app->input.grip_value_down[hand] ? GRIP_RELEASE_THRESHOLD
                                                 : GRIP_PRESS_THRESHOLD);
        grips[hand] =
            held(&hands[hand].squeeze) || app->input.grip_value_down[hand];
    }
}
