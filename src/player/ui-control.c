#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

#define GRAB_RECENTER_COS 0.5f
/* Hovering or dragging the seek bar: the position follows the pointer only
 * once it moved BAR_HYSTERESIS_PX (panel pixels) away, and the preview
 * redraws the panel at most every PREVIEW_REDRAW_INTERVAL seconds. The
 * hand's jitter otherwise redrew it (several ms on the CPU) every frame. */
#define BAR_HYSTERESIS_PX 5.0f
#define PREVIEW_REDRAW_INTERVAL 0.033

/* Steps of 10 %; above 100 % the samples are amplified (may clip). */
void change_volume(struct app *app, int direction)
{
    float volume = roundf((app->volume + 0.1f * (float)direction) * 10.0f) /
                   10.0f;
    app->volume = volume < 0.0f ? 0.0f : volume > 1.5f ? 1.5f : volume;
    settings_set_double("volume", app->volume);
    send_volume(app);
    app->ui_dirty = true;
}

/* Applies a choice from the view popup at once and reports it to the
 * session, which stores it for this title. */
static void change_view(struct app *app, int control)
{
    if (control >= PLAYER_UI_PROJECTION &&
        control <= PLAYER_UI_PROJECTION + NV12_PROJECTION_FISHEYE) {
        uint32_t projection = (uint32_t)(control - PLAYER_UI_PROJECTION);
        /* Choosing fisheye again steps through the usual lens angles. */
        static const uint32_t lenses[] = {180, 190, 200, 220};
        if (projection == NV12_PROJECTION_FISHEYE &&
            app->projection_mode == NV12_PROJECTION_FISHEYE) {
            size_t next = 0;
            for (size_t i = 0; i < sizeof(lenses) / sizeof(lenses[0]); ++i)
                if (lenses[i] == app->fisheye_fov)
                    next = (i + 1) % (sizeof(lenses) / sizeof(lenses[0]));
            app->fisheye_fov = lenses[next];
        }
        app->projection_mode = projection;
        app->format_source = NV12_FORMAT_SOURCE_USER;
    } else if (control >= PLAYER_UI_STEREO &&
               control <= PLAYER_UI_STEREO + NV12_STEREO_TB) {
        app->stereo_layout = (uint32_t)(control - PLAYER_UI_STEREO);
        app->format_source = NV12_FORMAT_SOURCE_USER;
    } else if (control == PLAYER_UI_SWAP_EYES)
        app->swap_eyes = !app->swap_eyes;
    else if (control >= PLAYER_UI_LEVELS &&
             control <= PLAYER_UI_LEVELS + NV12_LEVELS_FULL)
        app->video_levels = (uint32_t)(control - PLAYER_UI_LEVELS);
    else
        return;
    app->ui_dirty = true;
    struct nv12_ipc_message message = {
        .type = NV12_IPC_VIEW,
        .projection_mode = app->projection_mode,
        .fisheye_fov = app->fisheye_fov,
        .stereo_layout = app->stereo_layout,
        .swap_eyes = app->swap_eyes,
        .video_levels = app->video_levels,
    };
    send_message(app, &message);
    printf("player_view projection=%u stereo=%u swap_eyes=%u levels=%u\n",
           app->projection_mode, app->stereo_layout, app->swap_eyes,
           app->video_levels);
}

/* Per-title values go to the title's entry in titles.json, tilt and head
 * motion to settings.json. */
void save_adjust(const struct app *app, int index)
{
    const struct player_adjust_info *info = &player_adjust_info[index];
    if (info->global) {
        settings_set_double(info->key, app->adjust[index]);
        return;
    }
    if (!app->title_key[0])
        return;
    const char *names[PLAYER_ADJUST_COUNT];
    for (int i = 0; i < PLAYER_ADJUST_COUNT; ++i)
        names[i] = player_adjust_info[i].global ? NULL
                                                : player_adjust_info[i].key;
    if (title_settings_save_numbers(app->title_key, "adjust", names,
                                    app->adjust, PLAYER_ADJUST_COUNT) < 0)
        fprintf(stderr, "picture adjustments could not be saved\n");
}

static float clamp_adjust(int index, float value)
{
    const struct player_adjust_info *info = &player_adjust_info[index];
    return value < info->minimum ? info->minimum
           : value > info->maximum ? info->maximum : value;
}

/* Global values at startup, per-title values when a title starts. */
void load_adjust(struct app *app, bool global)
{
    const char *names[PLAYER_ADJUST_COUNT];
    for (int i = 0; i < PLAYER_ADJUST_COUNT; ++i) {
        const struct player_adjust_info *info = &player_adjust_info[i];
        names[i] = info->global ? NULL : info->key;
        if (info->global == global)
            app->adjust[i] = info->standard;
        if (global && info->global)
            app->adjust[i] = clamp_adjust(
                i, (float)settings_get_double(info->key, info->standard));
    }
    if (!global && app->title_key[0]) {
        float values[PLAYER_ADJUST_COUNT];
        memcpy(values, app->adjust, sizeof(values));
        if (title_settings_load_numbers(app->title_key, "adjust", names,
                                        values, PLAYER_ADJUST_COUNT) > 0)
            for (int i = 0; i < PLAYER_ADJUST_COUNT; ++i)
                if (!player_adjust_info[i].global)
                    app->adjust[i] = clamp_adjust(i, values[i]);
    }
}

void change_adjust(struct app *app, int control)
{
    int index = (control - PLAYER_UI_ADJUST) / 2;
    int direction = (control - PLAYER_UI_ADJUST) % 2 ? 1 : -1;
    app->adjust[index] = player_adjust_step((enum player_adjust)index,
                                            app->adjust[index], direction);
    save_adjust(app, index);
    app->ui_dirty = true;
    printf("player_adjust %s=%.3f\n", player_adjust_info[index].key,
           app->adjust[index]);
}

/* "Reset" of the position or the picture tab. */
static void reset_adjust(struct app *app, bool picture)
{
    for (int i = 0; i < PLAYER_ADJUST_COUNT; ++i) {
        bool is_picture = i >= PLAYER_ADJUST_BRIGHTNESS &&
                          i <= PLAYER_ADJUST_GAMMA;
        bool is_position = i <= PLAYER_ADJUST_STEREO_Y;
        if (picture ? !is_picture : !is_position)
            continue;
        app->adjust[i] = player_adjust_info[i].standard;
        save_adjust(app, i);
    }
    app->ui_dirty = true;
    printf("player_adjust_reset picture=%d\n", picture);
}

/* Scrub preview settings from the view popup; global, kept in
 * settings.json. */
static void change_preview(struct app *app, int control)
{
    if (control == PLAYER_UI_PREVIEW_MODE ||
        control == PLAYER_UI_PREVIEW_MODE + 1) {
        app->preview_perspective = control == PLAYER_UI_PREVIEW_MODE + 1;
        settings_set_bool("trickplay_perspective", app->preview_perspective);
    } else if (control >= PLAYER_UI_PREVIEW_FOV &&
               control < PLAYER_UI_PREVIEW_FOV + PLAYER_UI_PREVIEW_FOV_COUNT) {
        app->preview_fov =
            player_ui_preview_fovs[control - PLAYER_UI_PREVIEW_FOV];
        app->preview_perspective = true;
        settings_set_bool("trickplay_perspective", true);
        settings_set_int("trickplay_fov", app->preview_fov);
    } else {
        return;
    }
    app->ui_dirty = true;
    printf("player_preview perspective=%d fov=%d\n",
           app->preview_perspective, app->preview_fov);
}

void load_preview_settings(struct app *app)
{
    app->preview_perspective = settings_get_bool("trickplay_perspective",
                                                 true);
    int fov = settings_get_int("trickplay_fov", 100);
    app->preview_fov = 100;
    for (int i = 0; i < PLAYER_UI_PREVIEW_FOV_COUNT; ++i)
        if (player_ui_preview_fovs[i] == fov)
            app->preview_fov = fov;
    int autohide = settings_get_int("panel_autohide", 5);
    app->autohide = 0;
    for (int i = 0; i < PLAYER_UI_AUTOHIDE_COUNT; ++i)
        if (player_ui_autohide[i] == autohide)
            app->autohide = autohide;
    int end_action = settings_get_int("end_action", PLAYER_END_STOP);
    app->end_action = end_action >= 0 && end_action < PLAYER_END_COUNT
                          ? end_action
                          : PLAYER_END_STOP;
    double volume = settings_get_double("volume", 1.0);
    app->volume = volume < 0.0 ? 0.0f : volume > 1.5 ? 1.5f : (float)volume;
    app->focus_threshold = settings_get_bool("focus_threshold", false);
    app->av1_software = settings_get_bool("av1_software", true);
    app->passthrough = settings_get_bool("passthrough", false);
    for (int i = 0; i < BINDING_ACTION_COUNT; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "binding_%s", binding_action_info[i].key);
        app->controls.bindings[i] = binding_decode(settings_get_int(
            key, binding_encode(binding_action_info[i].standard)));
    }
}

struct player_ui_state settings_state(const struct app *app)
{
    struct player_ui_state state = player_state(app);
    state.settings_page = true;
    state.popup_open = true;
    state.popup_tab = app->settings_tab;
    state.hover = app->settings_hover;
    return state;
}

struct player_ui_state player_state(const struct app *app)
{
    return (struct player_ui_state){
        .title = app->title,
        .position_us = app->link.position_us,
        .duration_us = app->link.duration_us,
        .scrub_us = app->scrub_us,
        .scrubbing = app->scrub_hand >= 0 || app->seek_pending ||
                     app->seek_scrubbing,
        .loading = app->awaiting_frame,
        .paused = app->link.player_state == NV12_PLAYER_PAUSED,
        .grabbed = app->grab_hand >= 0,
        .popup_open = app->popup_open,
        .popup_tab = app->popup_tab,
        .adjust = app->adjust,
        .gaze_debug = app->gaze_debug,
        .focus_threshold = app->focus_threshold,
        .av1_software = app->av1_software,
        .passthrough = app->passthrough,
        .passthrough_supported = app->xr.alpha_blend_supported,
        .key_picking = app->key_picking,
        .language = i18n_choice(),
        .system_language = app->system_language,
        .update_installed = app->updater.installed,
        .update_latest = app->updater.latest,
        .update_status = app->update_status,
        .update_can_check = app->updater.usable &&
                            (app->updater.state == UPDATER_IDLE ||
                             app->updater.state == UPDATER_FAILED),
        .update_can_install =
            app->updater.usable && app->updater.latest[0] &&
            (app->updater.state == UPDATER_IDLE ||
             app->updater.state == UPDATER_FAILED) &&
            (updater_available(&app->updater) ||
             !app->updater.installed[0]),
        .update_auto = app->update_auto,
        .bindings = app->controls.bindings,
        .learn_action = app->controls.learn_action,
        .debug_text = app->gaze_debug ? app->autofocus.debug_text : NULL,
        .projection_mode = app->projection_mode,
        .stereo_layout = app->stereo_layout,
        .swap_eyes = app->swap_eyes,
        .video_levels = app->video_levels,
        .format_source = app->format_source,
        .fisheye_fov = app->fisheye_fov,
        .volume = app->volume,
        .end_action = app->end_action,
        .autohide = app->autohide,
        .details = app->details_valid ? &app->details : NULL,
        .hover = app->ui_hover,
        .preview_visible = app->preview_visible,
        .preview_us = app->preview_us,
        /* One picture unless the strip is wanted (the worker may still
         * hold a strip from before). */
        .preview = {app->preview_strip ? app->preview[0] : NULL,
                    app->preview_strip ? app->preview[1] : NULL,
                    app->preview[2],
                    app->preview_strip ? app->preview[3] : NULL,
                    app->preview_strip ? app->preview[4] : NULL},
        .preview_perspective = app->preview_perspective,
        .preview_fov = app->preview_fov,
    };
}

static int64_t scrub_target_us(const struct app *app, float u)
{
    return (int64_t)((double)player_ui_seek_ratio(u) *
                     (double)app->link.duration_us);
}

/* Hit on a visible part of the player panel (not its transparent area). */
static bool on_player_panel(const struct app *app,
                            const struct panel_hit *hit)
{
    struct player_ui_state state = player_state(app);
    return panel_hit_inside(hit) &&
           player_ui_on_panel(&state, hit->u, hit->v);
}

/* Show the panel again. If it is far outside the gaze cone it is moved in
 * front of the viewer, otherwise it keeps its world position. */
static void show_panel(struct app *app, const XrSpaceLocation *head,
                       bool head_valid)
{
    app->ui_visible = true;
    app->last_input_time = monotonic_seconds();
    if (head_valid) {
        XrVector3f gaze = rotate_vector(head->pose.orientation,
                                        (XrVector3f){0, 0, -1});
        XrVector3f to_panel = {app->panel_x - head->pose.position.x,
                               app->panel_y - head->pose.position.y,
                               app->panel_z - head->pose.position.z};
        float length = sqrtf(vector_dot(to_panel, to_panel));
        if (length < 0.1f ||
            vector_dot(gaze, to_panel) / length < GRAB_RECENTER_COS) {
            float flat = sqrtf(gaze.x * gaze.x + gaze.z * gaze.z);
            if (flat > 0.01f) {
                gaze.x /= flat;
                gaze.z /= flat;
                app->panel_x = head->pose.position.x + gaze.x * 1.3f;
                app->panel_y = head->pose.position.y - 0.35f;
                app->panel_z = head->pose.position.z + gaze.z * 1.3f;
                app->panel_yaw = atan2f(-gaze.x, -gaze.z);
            }
        }
    }
    app->ui_dirty = true;
    send_panel_state(app);
    printf("player_panel=visible x=%.3f y=%.3f z=%.3f yaw=%.3f\n",
           app->panel_x, app->panel_y, app->panel_z, app->panel_yaw);
}

/* Shows the scrub preview for `position_us` (-1 hides it), a strip or one
 * picture, and asks the trickplay worker for it. */
static void update_preview(struct app *app, int64_t position_us, bool strip)
{
    bool visible = position_us >= 0 && app->link.duration_us > 0;
    if (visible != app->preview_visible || strip != app->preview_strip)
        app->ui_dirty = true;
    else if (visible && position_us / 1000000 != app->preview_us / 1000000)
        app->preview_redraw_pending = true;
    double now = monotonic_seconds();
    if (app->ui_dirty) {
        app->preview_redraw_pending = false;
        app->preview_redraw_time = now;
    } else if (app->preview_redraw_pending &&
               now - app->preview_redraw_time >= PREVIEW_REDRAW_INTERVAL) {
        app->preview_redraw_pending = false;
        app->preview_redraw_time = now;
        app->ui_dirty = true;
    }
    app->preview_visible = visible;
    app->preview_strip = strip;
    if (!visible)
        return;
    app->preview_us = position_us;
    /* The strip steps as the seek bindings do: held, by the grown step. */
    int64_t spacing = !strip                ? 0
                      : app->seek_scrubbing ? app->preview_spacing_us
                                            : SEEK_STEP_US;
    trickplay_request(app->trickplay, position_us, spacing,
                      app->stereo_layout, app->projection_mode,
                      app->preview_perspective ? app->preview_fov : 0);
}

/* Seek bar position `u` (0..1 across the panel) with hysteresis, see
 * BAR_HYSTERESIS_PX. */
static float steady_bar_u(struct app *app, float u)
{
    if (app->bar_u < 0.0f ||
        fabsf(u - app->bar_u) * UI_WIDTH >= BAR_HYSTERESIS_PX)
        app->bar_u = u;
    return app->bar_u;
}

/* Status line of the updates tab and the dot on "Settings". */
void refresh_update_status(struct app *app)
{
    const struct updater *updater = &app->updater;
    char *text = app->update_status;
    const size_t size = sizeof(app->update_status);
    static const char *const phases[] = {
        [UPDATER_PHASE_DOWNLOAD] = N_("Downloading …"),
        [UPDATER_PHASE_VERIFY] = N_("Checking the signature …"),
        [UPDATER_PHASE_UNPACK] = N_("Unpacking …"),
        [UPDATER_PHASE_INSTALL] = N_("Installing …"),
    };
    text[0] = '\0';
    if (!updater->usable)
        snprintf(text, size, "%s",
                 tr("Updates are not set up in this installation."));
    else if (updater->state == UPDATER_CHECKING)
        snprintf(text, size, "%s", tr("Looking for a new version …"));
    else if (updater->state == UPDATER_INSTALLING)
        snprintf(text, size, "%s", tr(phases[updater->phase]));
    else if (updater->state == UPDATER_DONE)
        snprintf(text, size, "%s", tr("Installed. Stereonaut restarts …"));
    else if (updater->state == UPDATER_FAILED)
        snprintf(text, size, tr("Failed: %s"), updater_error(updater));
    else if (updater->checked && updater_available(updater))
        snprintf(text, size, tr("Version %s is available."),
                 updater->latest);
    else if (updater->checked && !updater->installed[0])
        snprintf(text, size, tr("Release %s can replace this build."),
                 updater->latest);
    else if (updater->checked)
        snprintf(text, size, "%s", tr("Stereonaut is up to date."));
    bool badge = updater_available(updater);
    if (badge != app->browser.settings_badge) {
        app->browser.settings_badge = badge;
        app->browser.content_dirty = true;
    }
    app->browser.dirty = true;
    app->ui_dirty = true;
}

/* Controls of the player popup and of the settings page over the browser:
 * global settings, controller bindings, picture values and the popup tabs.
 * False if `action` is none of them. */
static bool apply_setting(struct app *app, int action)
{
    if (action >= PLAYER_UI_BINDING &&
        action < PLAYER_UI_BINDING + BINDING_ACTION_COUNT) {
        int chosen = action - PLAYER_UI_BINDING;
        app->controls.learn_action =
            app->controls.learn_action == chosen ? -1 : chosen;
    } else if (action == PLAYER_UI_BINDING_CLEAR) {
        if (app->controls.learn_action >= 0) {
            app->controls.bindings[app->controls.learn_action] =
                (struct binding){-1, -1};
            save_binding(app, app->controls.learn_action);
        }
        app->controls.learn_action = -1;
    } else if (action == PLAYER_UI_BINDING_CANCEL) {
        app->controls.learn_action = -1;
    } else if (action == PLAYER_UI_BINDING_DEFAULTS) {
        for (int i = 0; i < BINDING_ACTION_COUNT; ++i) {
            app->controls.bindings[i] = binding_action_info[i].standard;
            save_binding(app, i);
        }
        app->controls.learn_action = -1;
    } else if (action >= PLAYER_UI_ADJUST &&
               action < PLAYER_UI_ADJUST_END) {
        change_adjust(app, action);
    } else if (action >= PLAYER_UI_TAB &&
               action < PLAYER_UI_TAB + PLAYER_UI_TAB_COUNT) {
        int tab = action - PLAYER_UI_TAB;
        if (tab >= PLAYER_UI_FIRST_SETTINGS_TAB)
            app->settings_tab = tab;
        else
            app->popup_tab = tab;
        app->controls.learn_action = -1;
    } else if (action >= PLAYER_UI_AUTOHIDE &&
               action < PLAYER_UI_AUTOHIDE + PLAYER_UI_AUTOHIDE_COUNT) {
        app->autohide = player_ui_autohide[action - PLAYER_UI_AUTOHIDE];
        settings_set_int("panel_autohide", app->autohide);
    } else if (action == PLAYER_UI_FOCUS_THRESHOLD ||
               action == PLAYER_UI_FOCUS_THRESHOLD + 1) {
        app->focus_threshold = action == PLAYER_UI_FOCUS_THRESHOLD + 1;
        settings_set_bool("focus_threshold", app->focus_threshold);
        printf("focus_threshold=%d\n", app->focus_threshold);
    } else if (action == PLAYER_UI_AV1_SOFTWARE ||
               action == PLAYER_UI_AV1_SOFTWARE + 1) {
        app->av1_software = action == PLAYER_UI_AV1_SOFTWARE + 1;
        settings_set_bool("av1_software", app->av1_software);
        printf("av1_software=%d\n", app->av1_software);
    } else if (action == PLAYER_UI_PASSTHROUGH ||
               action == PLAYER_UI_PASSTHROUGH + 1) {
        app->passthrough = action == PLAYER_UI_PASSTHROUGH + 1;
        settings_set_bool("passthrough", app->passthrough);
        printf("passthrough=%d supported=%d\n", app->passthrough,
               app->xr.alpha_blend_supported);
    } else if (action == PLAYER_UI_KEY_PICK) {
        app->key_picking = !app->key_picking;
        app->autofocus.key_pick_request = false;
        printf("key_picking=%d\n", app->key_picking);
    } else if (action >= PLAYER_UI_KEY_COLOR &&
               action <= PLAYER_UI_KEY_COLOR + 3) {
        app->adjust[PLAYER_ADJUST_KEY_COLOR] =
            (float)(action - PLAYER_UI_KEY_COLOR);
        save_adjust(app, PLAYER_ADJUST_KEY_COLOR);
        printf("player_adjust key_color=%d\n",
               action - PLAYER_UI_KEY_COLOR);
    } else if (action == PLAYER_UI_PASSTHROUGH_TOGGLE) {
        app->passthrough = !app->passthrough;
        settings_set_bool("passthrough", app->passthrough);
        printf("passthrough=%d supported=%d\n", app->passthrough,
               app->xr.alpha_blend_supported);
    } else if ((action >= PLAYER_UI_PREVIEW_MODE &&
                action <= PLAYER_UI_PREVIEW_MODE + 1) ||
               (action >= PLAYER_UI_PREVIEW_FOV &&
                action < PLAYER_UI_PREVIEW_FOV +
                             PLAYER_UI_PREVIEW_FOV_COUNT)) {
        change_preview(app, action);
    } else if (action >= PLAYER_UI_LANGUAGE &&
               action < PLAYER_UI_LANGUAGE_END) {
        int language = action - PLAYER_UI_LANGUAGE - 1;
        if (language >= i18n_language_count)
            return false;
        const char *choice =
            language < 0 ? "auto" : i18n_languages[language]->code;
        settings_set_string("language", choice);
        app->system_language = i18n_detect();
        int shown = i18n_select(choice);
        printf("language=%s choice=%s\n", i18n_languages[shown]->code,
               choice);
        browser_ui_relocalize(&app->browser);
        refresh_update_status(app);
    } else if (action == PLAYER_UI_UPDATE_CHECK) {
        updater_check(&app->updater);
        refresh_update_status(app);
    } else if (action == PLAYER_UI_UPDATE_INSTALL) {
        updater_install(&app->updater);
        refresh_update_status(app);
    } else if (action == PLAYER_UI_UPDATE_AUTO ||
               action == PLAYER_UI_UPDATE_AUTO + 1) {
        app->update_auto = action == PLAYER_UI_UPDATE_AUTO + 1;
        settings_set_bool("update_check", app->update_auto);
    } else {
        return false;
    }
    app->ui_dirty = true;
    app->browser.dirty = true;
    return true;
}

void sync_player_input(struct app *app, XrTime time)
{
    XrActiveActionSet active = {.actionSet = app->input.action_set};
    XrActionsSyncInfo sync = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    need_xr("xrSyncActions", xrSyncActions(app->xr.session, &sync));
    XrSpaceLocation head = {.type = XR_TYPE_SPACE_LOCATION};
    need_xr("xrLocateSpace(player head)",
            xrLocateSpace(app->xr.view_space, app->xr.space, time, &head));
    const XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool head_valid = (head.locationFlags & valid) == valid;

    struct hand_input hands[2] = {0};
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo pose_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->input.aim_action,
            .subactionPath = app->input.hands[hand],
        };
        XrActionStatePose pose = {.type = XR_TYPE_ACTION_STATE_POSE};
        need_xr("xrGetActionStatePose",
                xrGetActionStatePose(app->xr.session, &pose_info, &pose));
        if (pose.isActive) {
            XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
            need_xr("xrLocateSpace(player aim)",
                    xrLocateSpace(app->input.aim_spaces[hand], app->xr.space,
                                  time, &location));
            if ((location.locationFlags & valid) == valid) {
                hands[hand].aim_valid = true;
                hands[hand].aim = location.pose;
                hands[hand].hit = panel_ray(app, location.pose);
            }
        }
        read_select(app, hand, &hands[hand]);
        read_boolean(app, app->input.squeeze_action, app->input.hands[hand],
                     &hands[hand].squeeze);
    }
    bool grips[2];
    read_grips(app, hands, grips);
    process_bindings(app, grips);
    if (app->seek_show_panel) {
        app->seek_show_panel = false;
        if (!app->ui_visible) {
            show_panel(app, &head, head_valid);
            app->auto_hidden = false;
        }
    }
    /* Seeking with the bindings: seek once the input rests, and play on
     * from there if it played before. */
    if (app->seek_scrubbing &&
        monotonic_seconds() - app->seek_step_time >= SEEK_SCRUB_SETTLE) {
        app->seek_scrubbing = false;
        app->seek_pending = true;
        send_seek_to(app, app->scrub_us,
                     app->seek_resume ? NV12_PLAYER_PLAYING : 0);
        printf("seek_scrub=done target_us=%" PRId64 " resume=%d\n",
               app->scrub_us, app->seek_resume);
        app->seek_resume = false;
    }
    for (unsigned hand = 0; hand < 2; ++hand)
        app->overlay.lasers[hand] = (struct laser){
            .valid = hands[hand].aim_valid,
            .aim = hands[hand].aim,
            .panel_length = app->ui_visible &&
                                    on_player_panel(app, &hands[hand].hit)
                                ? hands[hand].hit.distance
                                : -1.0f,
        };
    app->overlay.wanted = app->ui_visible || app->key_picking;

    /* Auto-hide: any trigger, grip or stick, or pointing at the panel,
     * counts as input; a trigger brings a hidden panel back without
     * pressing whatever lies under the pointer. */
    double now = monotonic_seconds();
    bool consumed_select = false;
    for (unsigned hand = 0; hand < 2; ++hand) {
        bool pointing = app->ui_visible && hands[hand].aim_valid &&
                        on_player_panel(app, &hands[hand].hit);
        if (hands[hand].select_edge || pressed_edge(&hands[hand].squeeze) ||
            pointing)
            app->last_input_time = now;
        if (app->auto_hidden && !app->ui_visible &&
            hands[hand].select_edge) {
            show_panel(app, &head, head_valid);
            app->auto_hidden = false;
            consumed_select = true;
        }
    }
    if (app->ui_visible && app->autohide > 0 &&
        app->link.player_state == NV12_PLAYER_PLAYING && !app->popup_open &&
        !app->awaiting_frame && app->grab_hand < 0 && app->scrub_hand < 0 &&
        !app->seek_scrubbing && now - app->last_input_time > app->autohide) {
        app->ui_visible = false;
        app->auto_hidden = true;
        send_panel_state(app);
        printf("player_panel=auto-hidden after_s=%d\n", app->autohide);
    }

    /* Grip: grab the panel, or show/hide it when pressed on empty space. */
    if (app->grab_hand >= 0 &&
        !held(&hands[app->grab_hand].squeeze)) {
        app->grab_hand = -1;
        app->ui_dirty = true;
        send_panel_state(app);
        printf("player_panel=released x=%.3f y=%.3f z=%.3f yaw=%.3f\n",
               app->panel_x, app->panel_y, app->panel_z, app->panel_yaw);
    }
    for (unsigned hand = 0; hand < 2; ++hand) {
        /* Show/hide on release, unless the grip served as a modifier of a
         * button binding in between. */
        if (app->controls.grip_toggle[hand] && !held(&hands[hand].squeeze)) {
            app->controls.grip_toggle[hand] = false;
            if (app->ui_visible) {
                app->ui_visible = false;
                app->auto_hidden = false;
                send_panel_state(app);
                printf("player_panel=hidden\n");
            } else {
                show_panel(app, &head, head_valid);
                app->auto_hidden = false;
            }
        }
        if (!pressed_edge(&hands[hand].squeeze) || app->grab_hand >= 0 ||
            app->scrub_hand >= 0)
            continue;
        if (app->ui_visible && hands[hand].aim_valid &&
            on_player_panel(app, &hands[hand].hit)) {
            XrPosef aim = hands[hand].aim;
            XrVector3f center = {app->panel_x - aim.position.x,
                                 app->panel_y - aim.position.y,
                                 app->panel_z - aim.position.z};
            app->grab_offset = rotate_vector(
                quaternion_conjugate(aim.orientation), center);
            app->grab_hand = (int)hand;
            app->ui_dirty = true;
            printf("player_panel=grabbed hand=%u\n", hand);
        } else if (app->controls.learn_action < 0) {
            app->controls.grip_toggle[hand] = true;
        }
    }
    if (app->grab_hand >= 0 && hands[app->grab_hand].aim_valid) {
        XrPosef aim = hands[app->grab_hand].aim;
        XrVector3f offset = rotate_vector(aim.orientation, app->grab_offset);
        app->panel_x = aim.position.x + offset.x;
        app->panel_y = aim.position.y + offset.y;
        app->panel_z = aim.position.z + offset.z;
        if (head_valid)
            app->panel_yaw = atan2f(head.pose.position.x - app->panel_x,
                                    head.pose.position.z - app->panel_z);
    }

    /* Trigger: buttons on press, seek bar while held. */
    if (app->scrub_hand >= 0) {
        struct hand_input *input = &hands[app->scrub_hand];
        if (input->aim_valid && input->hit.plane) {
            int64_t target =
                scrub_target_us(app, steady_bar_u(app, input->hit.u));
            if (target != app->scrub_us) {
                app->scrub_us = target;
                app->ui_dirty = true;
            }
        }
        if (!input->select_held) {
            printf("player_seek_bar target_us=%" PRId64 "\n", app->scrub_us);
            app->scrub_hand = -1;
            app->seek_pending = true;
            app->seek_by_binding = false;
            app->ui_dirty = true;
            send_seek_to(app, app->scrub_us, 0);
        }
    }
    /* Key colour picker: the trigger aimed at the video, not the panel;
     * render_frame measures the colour there. */
    for (unsigned hand = 0; app->key_picking && hand < 2; ++hand) {
        if (consumed_select || !hands[hand].select_edge ||
            !hands[hand].aim_valid ||
            (app->ui_visible && on_player_panel(app, &hands[hand].hit)))
            continue;
        app->autofocus.key_pick_aim = hands[hand].aim;
        app->autofocus.key_pick_request = true;
        consumed_select = true;
    }
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (!app->ui_visible || consumed_select || app->grab_hand >= 0 ||
            app->scrub_hand >= 0 || app->seek_pending ||
            !hands[hand].select_edge || !hands[hand].aim_valid ||
            !on_player_panel(app, &hands[hand].hit))
            continue;
        struct player_ui_state state = player_state(app);
        int action = player_ui_hit_test(&state, hands[hand].hit.u,
                                        hands[hand].hit.v);
        if (action == PLAYER_UI_SEEK_BAR) {
            app->scrub_hand = (int)hand;
            app->bar_u = -1.0f; /* starts where the trigger was pulled */
            app->scrub_us =
                scrub_target_us(app, steady_bar_u(app, hands[hand].hit.u));
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_VIEW_POPUP) {
            app->popup_open = !app->popup_open;
            app->controls.learn_action = -1;
            app->ui_dirty = true;
        } else if (apply_setting(app, action)) {
            /* global setting or popup tab */
        } else if (action == PLAYER_UI_END_ACTION) {
            app->end_action = (app->end_action + 1) % PLAYER_END_COUNT;
            settings_set_int("end_action", app->end_action);
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_VOLUME_DOWN ||
                   action == PLAYER_UI_VOLUME_UP) {
            change_volume(app, action == PLAYER_UI_VOLUME_UP ? 1 : -1);
        } else if (action == PLAYER_UI_GAZE_DEBUG) {
            app->gaze_debug = !app->gaze_debug;
            app->autofocus.debug_text[0] = '\0';
            app->ui_dirty = true;
            printf("gaze_debug=%d\n", app->gaze_debug);
        } else if (action == PLAYER_UI_RECENTER) {
            app->recenter_pending = true;
            app->recenter_yaw = true;
        } else if (action == PLAYER_UI_RESET_POSITION ||
                   action == PLAYER_UI_RESET_PICTURE) {
            reset_adjust(app, action == PLAYER_UI_RESET_PICTURE);
        } else if (action >= PLAYER_UI_PREVIEW_MODE) {
            change_preview(app, action);
        } else if (action >= PLAYER_UI_PROJECTION) {
            change_view(app, action);
        } else if (action == PLAYER_UI_SEEK_BACK ||
                   action == PLAYER_UI_SEEK_FORWARD) {
            seek_binding(app, action == PLAYER_UI_SEEK_FORWARD ? 1 : -1,
                         false);
        } else if (action) {
            send_ipc(app, NV12_IPC_ACTION, (uint32_t)action);
            /* Before the first frame the session may be blocked opening the
             * source and cannot read the action. */
            if (action == NV12_ACTION_BACK && app->awaiting_frame)
                send_control(app, PLAYER_UI_STOP_REQUESTED, NULL, 0);
        }
    }

    int hover = 0;
    int64_t hover_us = -1;
    bool pointer_visible = false;
    int pointer_x = 0;
    int pointer_y = 0;
    app->panels.pointer_layer_visible = false;
    if (app->ui_visible && app->grab_hand < 0) {
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (!hands[hand].aim_valid ||
                !on_player_panel(app, &hands[hand].hit))
                continue;
            pointer_visible = true;
            pointer_x = (int)(hands[hand].hit.u * UI_WIDTH);
            pointer_y = (int)(hands[hand].hit.v * UI_HEIGHT);
            struct player_ui_state state = player_state(app);
            hover = player_ui_hit_test(&state, hands[hand].hit.u,
                                       hands[hand].hit.v);
            if (hover == PLAYER_UI_SEEK_BAR)
                hover_us = scrub_target_us(
                    app, steady_bar_u(app, hands[hand].hit.u));
            place_pointer(app,
                          (XrVector3f){app->panel_x, app->panel_y,
                                       app->panel_z},
                          app->panel_yaw, UI_PANEL_WIDTH_METERS,
                          UI_PANEL_HEIGHT_METERS, hands[hand].hit, 0.03f);
        }
        if (app->scrub_hand >= 0)
            hover = PLAYER_UI_SEEK_BAR;
    }
    if (hover_us < 0 && app->scrub_hand < 0)
        app->bar_u = -1.0f;
    if (hover != app->ui_hover) {
        app->ui_hover = hover;
        app->ui_dirty = true;
    }
    /* While a seek is pending or the bindings scrub, the preview shows
     * the target (only with the panel visible: it is part of it). */
    bool strip = false;
    if (app->scrub_hand >= 0) {
        hover_us = app->scrub_us;
    } else if (app->ui_visible && (app->seek_pending || app->seek_scrubbing)) {
        hover_us = app->scrub_us;
        strip = app->seek_by_binding;
    }
    update_preview(app, hover_us, strip);
    if (pointer_visible != app->ui_pointer_visible ||
        (pointer_visible &&
         (pointer_x != app->ui_pointer_x ||
          pointer_y != app->ui_pointer_y))) {
        app->ui_pointer_visible = pointer_visible;
        app->ui_pointer_x = pointer_x;
        app->ui_pointer_y = pointer_y;
    }
}

void sync_browser_input(struct app *app, XrTime time)
{
    XrActiveActionSet active = {.actionSet = app->input.action_set};
    XrActionsSyncInfo sync = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    need_xr("xrSyncActions(browser)", xrSyncActions(app->xr.session, &sync));
    const XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool pointer_visible = false, pressed = false;
    app->panels.pointer_layer_visible = false;
    int pointer_x = 0, pointer_y = 0, hover = -1, settings_hover = -1;
    struct hand_input hands[2] = {0};
    app->overlay.lasers[0].valid = app->overlay.lasers[1].valid = false;
    app->overlay.wanted = true;
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo pose_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->input.aim_action,
            .subactionPath = app->input.hands[hand],
        };
        XrActionStatePose pose = {.type = XR_TYPE_ACTION_STATE_POSE};
        need_xr("xrGetActionStatePose(browser)",
                xrGetActionStatePose(app->xr.session, &pose_info, &pose));
        read_select(app, hand, &hands[hand]);
        read_boolean(app, app->input.squeeze_action, app->input.hands[hand],
                     &hands[hand].squeeze);
        if (hands[hand].select_edge)
            pressed = true;
        int flick = read_stick(app, hand);
        if (flick && !app->settings_open)
            browser_ui_flip(&app->browser, flick);
        if (!pose.isActive)
            continue;
        XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
        need_xr("xrLocateSpace(browser aim)",
                xrLocateSpace(app->input.aim_spaces[hand], app->xr.space, time,
                              &location));
        if ((location.locationFlags & valid) != valid)
            continue;
        struct panel_hit hit = panel_ray_at(
            browser_center(app), app->home_yaw,
            BROWSER_UI_PANEL_WIDTH_METERS, BROWSER_UI_PANEL_HEIGHT_METERS,
            location.pose);
        app->overlay.lasers[hand] = (struct laser){
            .valid = true, .aim = location.pose,
            .panel_length = panel_hit_inside(&hit) ? hit.distance : -1.0f,
        };
        if (panel_hit_inside(&hit)) {
            pointer_visible = true;
            pointer_x = (int)(hit.u * BROWSER_UI_WIDTH);
            pointer_y = (int)(hit.v * BROWSER_UI_HEIGHT);
            hover = browser_ui_hit_test(hit.u, hit.v);
            int page_x = pointer_x - SETTINGS_X, page_y = pointer_y - SETTINGS_Y;
            if (app->settings_open && page_x >= 0 &&
                page_x < PLAYER_UI_POPUP_W && page_y >= 0 &&
                page_y < PLAYER_UI_POPUP_H) {
                struct player_ui_state state = settings_state(app);
                settings_hover = player_ui_hit_test(
                    &state,
                    (float)(page_x + PLAYER_UI_POPUP_X) / PLAYER_UI_WIDTH,
                    (float)(page_y + PLAYER_UI_POPUP_Y) / PLAYER_UI_HEIGHT);
            }
            place_pointer(app, browser_center(app), app->home_yaw,
                          BROWSER_UI_PANEL_WIDTH_METERS,
                          BROWSER_UI_PANEL_HEIGHT_METERS, hit, 0.04f);
        }
    }
    if (app->settings_open) {
        /* The page is modal: a click beside it closes it. */
        browser_ui_set_pointer(&app->browser, pointer_visible, pointer_x,
                               pointer_y, -1);
        if (settings_hover != app->settings_hover) {
            app->settings_hover = settings_hover;
            app->browser.dirty = true;
        }
        if (app->controls.learn_action >= 0) {
            bool grips[2];
            read_grips(app, hands, grips);
            process_bindings(app, grips);
            if (app->controls.learn_action < 0)
                app->browser.dirty = true;
        }
        if (!pressed)
            return;
        if (settings_hover == PLAYER_UI_SETTINGS_CLOSE ||
            (settings_hover < 0 && app->controls.learn_action < 0)) {
            app->settings_open = false;
            app->controls.learn_action = -1;
            app->browser.dirty = true;
            printf("settings_page=closed\n");
        } else if (settings_hover > 0) {
            apply_setting(app, settings_hover);
        }
        return;
    }
    browser_ui_set_pointer(&app->browser, pointer_visible, pointer_x,
                           pointer_y, hover);
    if (!pressed || app->awaiting_command)
        return;
    char path[PLAYER_UI_PATH_MAX];
    switch (browser_ui_activate(&app->browser, path, sizeof(path))) {
    case BROWSER_UI_FILE_SELECTED:
        app->awaiting_command = true;
        snprintf(app->title, sizeof(app->title), "%s",
                 app->browser.selected_title);
        send_control(app, PLAYER_UI_FILE_SELECTED, path,
                     app->browser.selected_start_us);
        break;
    case BROWSER_UI_EXIT_REQUESTED:
        app->awaiting_command = true;
        send_control(app, PLAYER_UI_EXIT_REQUESTED, NULL, 0);
        app->quit = true;
        break;
    case BROWSER_UI_SETTINGS_REQUESTED:
        app->settings_open = true;
        app->settings_hover = -1;
        app->controls.learn_action = -1;
        memset(app->controls.held, 0, sizeof(app->controls.held));
        browser_ui_close_menus(&app->browser);
        app->browser.dirty = true;
        printf("settings_page=open\n");
        break;
    case BROWSER_UI_NONE:
        break;
    }
}
