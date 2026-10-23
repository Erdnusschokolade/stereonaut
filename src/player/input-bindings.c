#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

/* Holding a stick direction or button repeats its action (volume, tilt,
 * zoom) after the delay, then at the interval. */
#define BINDING_REPEAT_DELAY 0.5
#define BINDING_REPEAT_INTERVAL 0.15

void save_binding(const struct app *app, int action)
{
    char key[64];
    snprintf(key, sizeof(key), "binding_%s", binding_action_info[action].key);
    settings_set_int(key, binding_encode(app->controls.bindings[action]));
    char text[64];
    binding_describe(app->controls.bindings[action], text, sizeof(text));
    printf("binding %s=%s\n", binding_action_info[action].key, text);
}

/* Ends the session and starts the next or previous title of the folder;
 * the BROWSE that follows picks up app->skip_path. */
static void skip_title(struct app *app, int direction)
{
    char next[PLAYER_UI_PATH_MAX];
    if (app->skip_path[0] ||
        !browser_ui_following_title(&app->browser, app->location, direction,
                                    next, sizeof(next))) {
        printf("binding_skip direction=%d none\n", direction);
        return;
    }
    snprintf(app->skip_path, sizeof(app->skip_path), "%s", next);
    snprintf(app->skip_title, sizeof(app->skip_title), "%s",
             app->browser.selected_title);
    printf("binding_skip direction=%d next=%s\n", direction, next);
    send_ipc(app, NV12_IPC_ACTION, NV12_ACTION_BACK);
    if (app->awaiting_frame)
        send_control(app, PLAYER_UI_STOP_REQUESTED, NULL, 0);
}

/* One step of seeking with the bindings or the panel's -10/+10 buttons,
 * see SEEK_BURST_GAP. Targets are absolute and continue from a pending
 * one, so the session lands where the seek bar shows. */
void seek_binding(struct app *app, int direction, bool repeated)
{
    if (app->scrub_hand >= 0)
        return;
    if (app->link.duration_us <= 0) {
        /* No status yet (loading): only the session knows the position. */
        send_ipc(app, NV12_IPC_ACTION,
                 direction > 0 ? NV12_ACTION_SEEK_FORWARD
                               : NV12_ACTION_SEEK_BACK);
        return;
    }
    double now = monotonic_seconds();
    int64_t step = SEEK_STEP_US;
    if (repeated) {
        double held_for = now - app->seek_press_time;
        step *= held_for > 5.0 ? 6 : held_for > 2.5 ? 3 : 1;
    } else {
        app->seek_burst = now - app->seek_press_time < SEEK_BURST_GAP
                              ? app->seek_burst + 1
                              : 1;
        app->seek_press_time = now;
    }
    app->seek_step_time = now;
    app->preview_spacing_us = step;
    int64_t target = app->seek_pending || app->seek_scrubbing
                         ? app->scrub_us
                         : app->link.position_us;
    target += direction * step;
    if (target >= app->link.duration_us)
        target = app->link.duration_us - 1;
    if (target < 0)
        target = 0;
    app->scrub_us = target;
    app->seek_by_binding = true;
    app->ui_dirty = true;
    if (!app->seek_scrubbing &&
        (repeated || app->seek_burst >= SEEK_BURST_SCRUB)) {
        app->seek_scrubbing = true;
        app->seek_resume = app->link.player_state == NV12_PLAYER_PLAYING;
        struct nv12_ipc_message pause = {
            .type = NV12_IPC_ACTION, .index = NV12_ACTION_TOGGLE_PAUSE,
            .player_state = NV12_PLAYER_PAUSED,
        };
        send_message(app, &pause);
        app->seek_show_panel = true;
        printf("seek_scrub=start target_us=%" PRId64 "\n", target);
    }
    if (app->seek_scrubbing) {
        app->last_input_time = now;
        return;
    }
    app->seek_pending = true;
    send_seek_to(app, target, 0);
}

static void run_binding(struct app *app, int action, bool repeated)
{
    printf("binding_action %s\n", binding_action_info[action].key);
    switch (action) {
    case BINDING_SEEK_BACK:
    case BINDING_SEEK_FORWARD:
        seek_binding(app, action == BINDING_SEEK_FORWARD ? 1 : -1, repeated);
        break;
    case BINDING_VOLUME_DOWN:
    case BINDING_VOLUME_UP:
        change_volume(app, action == BINDING_VOLUME_UP ? 1 : -1);
        break;
    case BINDING_TILT_DOWN:
    case BINDING_TILT_UP:
        change_adjust(app, PLAYER_UI_ADJUST + 2 * PLAYER_ADJUST_TILT +
                               (action == BINDING_TILT_UP));
        break;
    case BINDING_ZOOM_OUT:
    case BINDING_ZOOM_IN:
        change_adjust(app, PLAYER_UI_ADJUST + 2 * PLAYER_ADJUST_ZOOM +
                               (action == BINDING_ZOOM_IN));
        break;
    case BINDING_PLAY_PAUSE:
        send_ipc(app, NV12_IPC_ACTION, NV12_ACTION_TOGGLE_PAUSE);
        break;
    case BINDING_PREVIOUS:
    case BINDING_NEXT:
        skip_title(app, action == BINDING_NEXT ? 1 : -1);
        break;
    }
}

/* Assigns the pressed buttons to the action waiting for them; another
 * action with the same buttons loses them. */
static void learn_binding(struct app *app, int input, int modifier)
{
    printf("binding_learn input=%d modifier=%d grip_value=%.2f/%.2f\n", input,
           modifier, app->input.grip_value[0], app->input.grip_value[1]);
    for (int i = 0; i < BINDING_ACTION_COUNT; ++i)
        if (i != app->controls.learn_action &&
            app->controls.bindings[i].input == input &&
            app->controls.bindings[i].modifier == modifier) {
            app->controls.bindings[i] = (struct binding){-1, -1};
            save_binding(app, i);
        }
    app->controls.bindings[app->controls.learn_action] =
        (struct binding){input, modifier};
    save_binding(app, app->controls.learn_action);
    app->controls.learn_action = -1;
    app->ui_dirty = true;
}

/* A modifier counts if it was down before this frame. */
static void press_input(struct app *app, int input, double now)
{
    app->last_input_time = now;
    app->controls.combined[input] = false;
    app->controls.repeat[input] = 0;
    int modifier = -1;
    if (app->controls.learn_action >= 0) {
        for (int m = BINDING_FIRST_BUTTON; m < BINDING_INPUT_COUNT; ++m)
            if (m != input && app->controls.held[m]) {
                modifier = m;
                break;
            }
        if (modifier >= 0) {
            app->controls.combined[modifier] = true;
            learn_binding(app, input, modifier);
        } else if (!binding_is_button(input)) {
            learn_binding(app, input, -1);
        }
        /* A button alone is assigned on release: it may still become the
         * first button of a combination. */
    } else {
        int action = binding_match(app->controls.bindings, input,
                                   app->controls.held, &modifier);
        if (modifier >= 0)
            app->controls.combined[modifier] = true;
        else if (binding_is_button(input) &&
                 binding_is_modifier(app->controls.bindings, input))
            return; /* acts on release unless used as a modifier */
        if (action < 0)
            return;
        run_binding(app, action, false);
        if (binding_action_info[action].repeat) {
            app->controls.repeat[input] = action + 1;
            app->controls.repeat_time[input] = now + BINDING_REPEAT_DELAY;
        }
    }
    /* A grip that became a modifier does not show/hide the panel. */
    if (modifier == BINDING_LEFT_GRIP || modifier == BINDING_RIGHT_GRIP)
        app->controls.grip_toggle[modifier == BINDING_RIGHT_GRIP] = false;
}

static void release_input(struct app *app, int input)
{
    bool combined = app->controls.combined[input];
    app->controls.combined[input] = false;
    app->controls.repeat[input] = 0;
    if (!binding_is_button(input) || combined ||
        input == BINDING_LEFT_GRIP || input == BINDING_RIGHT_GRIP)
        return;
    if (app->controls.learn_action >= 0) {
        learn_binding(app, input, -1);
        return;
    }
    if (!binding_is_modifier(app->controls.bindings, input))
        return; /* acted on press */
    const bool none[BINDING_INPUT_COUNT] = {false};
    int modifier;
    int action = binding_match(app->controls.bindings, input, none, &modifier);
    if (action >= 0)
        run_binding(app, action, false);
}

/* Turns stick directions and buttons into actions; while an action waits
 * for its buttons, the next press is assigned to it instead. */
void process_bindings(struct app *app, const bool grip[2])
{
    bool down[BINDING_INPUT_COUNT] = {false};
    for (unsigned hand = 0; hand < 2; ++hand) {
        read_stick_directions(app, hand, down);
        down[hand ? BINDING_RIGHT_GRIP : BINDING_LEFT_GRIP] = grip[hand];
    }
    for (size_t i = 0; i < ARRAY_COUNT(button_inputs); ++i) {
        XrActionStateBoolean state;
        read_boolean(app, app->input.button_actions[i],
                     app->input.hands[button_inputs[i].hand], &state);
        down[button_inputs[i].input] = held(&state);
    }
    double now = monotonic_seconds();
    for (int input = 0; input < BINDING_INPUT_COUNT; ++input) {
        bool was = app->controls.held[input];
        if (down[input] && !was) {
            press_input(app, input, now);
        } else if (!down[input] && was) {
            release_input(app, input);
        } else if (down[input] && app->controls.repeat[input] &&
                   now >= app->controls.repeat_time[input]) {
            run_binding(app, app->controls.repeat[input] - 1, true);
            app->controls.repeat_time[input] = now + BINDING_REPEAT_INTERVAL;
        }
    }
    memcpy(app->controls.held, down, sizeof(down));
}
