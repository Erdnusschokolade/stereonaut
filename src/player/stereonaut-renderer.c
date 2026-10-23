/* Continuously render decoded DMA-BUF frames (Iris, dav1d or a PC hardware
 * decoder) on the validated OpenXR quad. */
#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

static uint64_t parse_u64(const char *text, const char *name)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end || value == 0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint64_t)value;
}

/* ---- picture position and adjustments ------------------------------- */

_Static_assert(TRICKPLAY_STRIP == PLAYER_UI_PREVIEW_STRIP,
               "trickplay strip and panel preview differ");

/* ---- controller bindings ---------------------------------------------- */

/* Follows the update script; once an update is installed the launcher
 * restarts Stereonaut (not in the middle of a title). */
static void poll_updater(struct app *app)
{
    if (updater_poll(&app->updater))
        refresh_update_status(app);
    if (app->updater.state == UPDATER_DONE && !app->playing &&
        !app->awaiting_command && !app->quit) {
        printf("update installed; restarting\n");
        app->awaiting_command = true;
        send_control(app, PLAYER_UI_RESTART_REQUESTED, NULL, 0);
        app->quit = true;
    }
}


/* ---- launcher control socket ---------------------------------------- */

void send_control(struct app *app, uint32_t type, const char *path,
                  int64_t start_us)
{
    struct player_ui_result result = {.type = type, .start_us = start_us};
    if (path)
        snprintf(result.path, sizeof(result.path), "%s", path);
    ssize_t written;
    do {
        written = send(app->ui_fd, &result, sizeof(result), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)sizeof(result))
        app->quit = true; /* the launcher is gone */
}

/* Back to the browser: drop every video import and reset player state. */
static void end_playback(struct app *app)
{
    app->popup_open = false;
    video_import_pending(app, true);
    need_vk("vkDeviceWaitIdle(end playback)",
            vkDeviceWaitIdle(app->gpu.device));
    discard_held_frame(app);
    for (uint32_t i = 0; i < app->video.buffer_count; ++i)
        destroy_video_buffer(app, &app->video.buffers[i]);
    if (app->link.fd >= 0)
        close(app->link.fd);
    app->link.fd = -1;
    app->playing = false;
    app->link.eos_received = false;
    app->video.current = -1;
    app->video.presented = false;
    app->awaiting_frame = true;
    app->seek_pending = false;
    app->seek_scrubbing = false;
    app->seek_resume = false;
    app->seek_show_panel = false;
    app->seek_burst = 0;
    app->grab_hand = -1;
    app->scrub_hand = -1;
    app->controls.learn_action = -1;
    memset(app->controls.held, 0, sizeof(app->controls.held));
    memset(app->controls.combined, 0, sizeof(app->controls.combined));
    memset(app->controls.repeat, 0, sizeof(app->controls.repeat));
    app->controls.grip_toggle[0] = app->controls.grip_toggle[1] = false;
    app->ui_hover = 0;
    app->ui_pointer_visible = false;
    app->link.player_state = NV12_PLAYER_PLAYING;
    printf("openxr_summary xr_frames=%" PRIu64
           " rendered_frames=%" PRIu64 " received_frames=%" PRIu64
           " unique_video_frames=%" PRIu64
           " repeated_video_frames=%" PRIu64
           " hidden_video_frames=%" PRIu64
           " pipeline_skipped_frames=%" PRIu64
           " released_frames=%" PRIu64
           " session_changes=%" PRIu64 "\n",
           app->xr_frames, app->rendered_frames, app->received_frames,
           app->unique_video_frames, app->repeated_video_frames,
           app->hidden_video_frames, app->pipeline_skipped_frames,
           app->released_frames, app->session_changes);
    app->xr_frames = app->rendered_frames = app->received_frames = 0;
    app->unique_video_frames = app->repeated_video_frames = 0;
    app->hidden_video_frames = app->pipeline_skipped_frames = 0;
    app->released_frames = 0;
    app->link.position_us = 0;
    app->link.duration_us = 0;
    app->video.width = app->video.height = 0;
    /* The trigger that pressed "back" is usually still held: ignore it
     * until a release so the browser does not react to it (it could
     * otherwise land on "Beenden" and close the program). */
    app->input.select[0].suppressed = app->input.select[1].suppressed = true;
    trickplay_open(app->trickplay, NULL);
    app->details_valid = false;
    for (int i = 0; i < TRICKPLAY_STRIP; ++i) {
        ui_image_free(app->preview[i]);
        app->preview[i] = NULL;
    }
    app->preview_visible = false;
    app->ui_dirty = true;
    app->browser.dirty = true;
    printf("playback_ended\n");
}

/* `location` is what the session plays; "jf:item:ID[:PARENT]" has
 * trickplay previews. */
static void begin_playback(struct app *app, int fd, const char *location)
{
    if (app->playing)
        end_playback(app);
    snprintf(app->location, sizeof(app->location), "%s", location);
    app->link.title_finished = false;
    app->last_input_time = monotonic_seconds();
    app->auto_hidden = false;
    if (strncmp(location, "jf:item:", 8) == 0) {
        char item[64] = "";
        size_t length = strcspn(location + 8, ":");
        if (length < sizeof(item))
            memcpy(item, location + 8, length);
        item[length < sizeof(item) ? length : 0] = '\0';
        trickplay_open(app->trickplay, item);
    }
    if (location[0])
        title_settings_key(location, app->title_key, sizeof(app->title_key));
    else
        app->title_key[0] = '\0';
    load_adjust(app, false);
    /* Centre the dome on the head at the first rendered frame, turned
     * towards home (kept across titles until the viewer centres again). */
    app->recenter_pending = true;
    app->recenter_yaw = false;
    app->dome_yaw = -app->home_yaw;
    app->autofocus.offset = 0.0f;
    app->autofocus.speed = 0.0f;
    app->autofocus.time = 0.0;
    app->autofocus.focused = false;
    app->autofocus.focus_disparity = app->autofocus.distance = 0.0f;
    app->link.fd = fd;
    app->settings_open = false;
    app->key_picking = app->autofocus.key_pick_request = false;
    app->autofocus.key_sample_pending = false;
    app->playing = true;
    app->awaiting_command = false;
    app->link.eos_received = false;
    app->ui_dirty = true;
    /* see end_playback */
    app->input.select[0].suppressed = app->input.select[1].suppressed = true;
    printf("playback_attached\n");
}

static void handle_control(struct app *app)
{
    for (;;) {
        struct player_ui_command command = {0};
        char control[CMSG_SPACE(sizeof(int))] = {0};
        struct iovec iov = {.iov_base = &command, .iov_len = sizeof(command)};
        struct msghdr received = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control),
        };
        ssize_t bytes = recvmsg(app->ui_fd, &received, MSG_DONTWAIT);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes <= 0) {
            app->quit = true; /* the launcher closed the socket */
            return;
        }
        if ((size_t)bytes != sizeof(command))
            continue;
        if (command.type == PLAYER_UI_PLAY) {
            struct cmsghdr *header = CMSG_FIRSTHDR(&received);
            if (header && header->cmsg_level == SOL_SOCKET &&
                header->cmsg_type == SCM_RIGHTS &&
                header->cmsg_len == CMSG_LEN(sizeof(int))) {
                int fd;
                memcpy(&fd, CMSG_DATA(header), sizeof(fd));
                command.path[sizeof(command.path) - 1] = '\0';
                begin_playback(app, fd, command.path);
            }
        } else if (command.type == PLAYER_UI_BROWSE) {
            bool finished = app->playing && app->link.title_finished;
            if (app->playing)
                end_playback(app);
            command.path[sizeof(command.path) - 1] = '\0';
            command.message[sizeof(command.message) - 1] = '\0';
            /* A title that played to its end may lead straight into the
             * next one (or itself) without showing the browser. */
            char next[PLAYER_UI_PATH_MAX];
            bool follow = false;
            if (app->skip_path[0] && !command.message[0]) {
                /* "Next/previous video" ended the session. */
                snprintf(next, sizeof(next), "%s", app->skip_path);
                snprintf(app->title, sizeof(app->title), "%s",
                         app->skip_title);
                follow = true;
            } else if (finished && !command.message[0] &&
                       app->end_action == PLAYER_END_REPEAT) {
                snprintf(next, sizeof(next), "%s", app->location);
                follow = next[0] != '\0';
            } else if (finished && !command.message[0] &&
                       app->end_action == PLAYER_END_NEXT) {
                follow = browser_ui_following_title(&app->browser,
                                                    app->location, 1, next,
                                                    sizeof(next));
                if (follow)
                    snprintf(app->title, sizeof(app->title), "%s",
                             app->browser.selected_title);
            }
            app->skip_path[0] = '\0';
            if (follow) {
                printf("player_end_action=%d next=%s\n", app->end_action,
                       next);
                app->awaiting_command = true;
                send_control(app, PLAYER_UI_FILE_SELECTED, next, 0);
                continue;
            }
            browser_ui_navigate(&app->browser, command.path, command.message);
            app->awaiting_command = false;
        }
    }
}

/* Takes the head pose as home: its position and the direction the face
 * points in. With `panel` the player panel moves in front of it. */
void set_home(struct app *app, XrPosef head, bool panel)
{
    XrVector3f forward = rotate_vector(head.orientation,
                                       (XrVector3f){0, 0, -1});
    XrVector3f up = rotate_vector(head.orientation, (XrVector3f){0, 1, 0});
    /* Looking down the top of the head points forward, looking up it
     * points back: the heading holds even for a headset woken up facing
     * the ceiling. */
    float x = forward.x - up.x * forward.y;
    float z = forward.z - up.z * forward.y;
    app->home_yaw = atan2f(-x, -z);
    app->home_position = head.position;
    app->home_pending = false;
    if (panel) {
        app->panel_x = head.position.x - sinf(app->home_yaw) * 1.3f;
        app->panel_y = head.position.y - 0.35f;
        app->panel_z = head.position.z - cosf(app->home_yaw) * 1.3f;
        app->panel_yaw = app->home_yaw;
    }
    printf("home x=%.3f y=%.3f z=%.3f yaw=%.1f\n", head.position.x,
           head.position.y, head.position.z,
           app->home_yaw * 180.0f / PI_F);
}

/* Sets home at the start and after SteamVR's recenter (once the changed
 * space applies), turning a playing title with it. */
static void update_home(struct app *app, XrTime time)
{
    if (!app->home_pending || time < app->home_after)
        return;
    XrSpaceLocation head = {.type = XR_TYPE_SPACE_LOCATION};
    need_xr("xrLocateSpace(home)",
            xrLocateSpace(app->xr.view_space, app->xr.space, time, &head));
    const XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((head.locationFlags & valid) != valid)
        return;
    set_home(app, head.pose, true);
    if (app->playing) {
        app->recenter_pending = true;
        app->recenter_yaw = false;
        app->dome_yaw = -app->home_yaw;
        send_panel_state(app);
    }
}

static void note_max(double *max, double value)
{
    if (value > *max)
        *max = value;
}

/* After xrWaitFrame: counts the frame and missed display periods and logs
 * the window when it is over. */
static void timing_frame(struct app *app, const XrFrameState *state,
                         double waited_ms)
{
    struct frame_timing *timing = &app->timing;
    double now = monotonic_seconds();
    if (timing->window_start == 0.0)
        timing->window_start = now;
    XrDuration period = state->predictedDisplayPeriod;
    if (timing->last_display && period > 0) {
        XrDuration delta = state->predictedDisplayTime - timing->last_display;
        if (delta > period * 3 / 2)
            timing->missed += (uint32_t)((delta + period / 2) / period) - 1U;
    }
    timing->last_display = state->predictedDisplayTime;
    timing->period_ms = period / 1e6;
    timing->frames++;
    timing->wait_ms += waited_ms;
    if (now - timing->window_start < FRAME_TIMING_INTERVAL)
        return;
    uint32_t rendered = timing->rendered ? timing->rendered : 1U;
    uint32_t still = timing->gpu_frames[0] ? timing->gpu_frames[0] : 1U;
    uint32_t copied = timing->gpu_frames[1] ? timing->gpu_frames[1] : 1U;
    uint32_t frames = timing->frames;
    printf("frame_timing frames=%u rendered=%u missed=%u period_ms=%.1f"
           " wait_ms=%.1f cpu_ms=%.1f/%.1f gpu_ms=%.1f/%.1f"
           " copy_frames=%u copy_gpu_ms=%.1f/%.1f ui=%u ui_ms=%.1f/%.1f\n",
           frames, timing->rendered, timing->missed, timing->period_ms,
           timing->wait_ms / frames, timing->cpu_ms / rendered,
           timing->cpu_max_ms, timing->gpu_ms[0] / still,
           timing->gpu_max_ms[0], timing->gpu_frames[1],
           timing->gpu_ms[1] / copied, timing->gpu_max_ms[1],
           timing->ui_redraws,
           timing->ui_redraws ? timing->ui_ms / timing->ui_redraws : 0.0,
           timing->ui_max_ms);
    *timing = (struct frame_timing){.window_start = now,
                                    .last_display = timing->last_display};
}

void timing_ui(struct app *app, double started)
{
    double ms = (monotonic_seconds() - started) * 1e3;
    app->timing.ui_redraws++;
    app->timing.ui_ms += ms;
    note_max(&app->timing.ui_max_ms, ms);
}

/* Frame statistics, the layers of the frame and xrEndFrame. */
static void end_frame(struct app *app, struct frame *f)
{
    app->timing.gpu_ms[f->copied_video] += f->gpu_ms;
    app->timing.gpu_frames[f->copied_video]++;
    note_max(&app->timing.gpu_max_ms[f->copied_video], f->gpu_ms);
    XrSwapchainImageReleaseInfo release = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    need_xr("xrReleaseSwapchainImage", xrReleaseSwapchainImage(app->xr.swapchain, &release));
    for (uint32_t eye = 0;
         f->overlay && !app->overlay.in_eye_images && eye < 2; ++eye)
        need_xr("xrReleaseSwapchainImage(overlay)",
                xrReleaseSwapchainImage(app->overlay.swapchains[eye],
                                        &release));
    XrCompositionLayerProjectionView overlay_views[2];
    for (uint32_t eye = 0; eye < 2; ++eye)
        overlay_views[eye] = (XrCompositionLayerProjectionView){
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .pose = f->views[eye].pose, .fov = f->views[eye].fov,
            .subImage = {.swapchain = app->overlay.swapchains[eye],
                         .imageRect = {.extent = {
                             (int32_t)app->overlay.width,
                             (int32_t)app->overlay.height}}},
        };
    XrCompositionLayerProjection overlay_layer = {
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->xr.space, .viewCount = 2, .views = overlay_views,
    };
    XrCompositionLayerProjectionView projection_views[2] = {{.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        projection_views[eye].pose = f->views[eye].pose; projection_views[eye].fov = f->views[eye].fov;
        projection_views[eye].subImage = (XrSwapchainSubImage){.swapchain = app->xr.swapchain, .imageRect = {.extent = {(int32_t)app->xr.width, (int32_t)app->xr.height}}, .imageArrayIndex = eye};
    }
    XrCompositionLayerProjection layer = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .layerFlags = passthrough_active(app)
                          ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                          : 0,
        .space = app->xr.space, .viewCount = 2, .views = projection_views};
    XrCompositionLayerQuad pointer_layer = make_pointer_layer(app);
    XrCompositionLayerQuad gaze_layer = make_gaze_marker_layer(app);
    const XrCompositionLayerBaseHeader *layers[5] = {
        (const XrCompositionLayerBaseHeader *)&layer,
    };
    uint32_t layer_count = 1;
    if (app->playing && app->autofocus.gaze_marker_visible)
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&gaze_layer;
    if (f->show_ui) {
        if (!app->panels.panels_in_eye_images)
            layers[layer_count++] =
                (const XrCompositionLayerBaseHeader *)&f->ui_layer;
    }
    if (f->overlay && !app->overlay.in_eye_images)
        layers[layer_count++] =
            (const XrCompositionLayerBaseHeader *)&overlay_layer;
    if (f->show_ui && app->panels.pointer_layer_visible &&
        !app->panels.panels_in_eye_images)
        layers[layer_count++] =
            (const XrCompositionLayerBaseHeader *)&pointer_layer;
    XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = f->state.predictedDisplayTime,
        .environmentBlendMode = environment_blend_mode(app),
        .layerCount = layer_count,
        .layers = layers};
    double ending = monotonic_seconds();
    need_xr("xrEndFrame", xrEndFrame(app->xr.session, &end));
    ++app->rendered_frames;
    double cpu_ms = (monotonic_seconds() - f->frame_started) * 1e3;
    if (cpu_ms > 50.0)
        printf("slow_frame ms=%.0f inputs=%.0f acquire=%.0f fence=%.0f"
               " record=%.0f submit=%.0f gpu=%.0f end=%.0f copy=%d\n",
               cpu_ms, (f->inputs_done - f->frame_started) * 1e3,
               (f->acquired - f->inputs_done) * 1e3,
               (f->fenced - f->acquired) * 1e3,
               (f->recorded - f->fenced) * 1e3,
               (f->submitted - f->recorded) * 1e3, f->gpu_ms,
               (monotonic_seconds() - ending) * 1e3, f->copied_video);
    app->timing.rendered++;
    app->timing.cpu_ms += cpu_ms;
    note_max(&app->timing.cpu_max_ms, cpu_ms);
    if (!f->video)
        return;
    if (app->video.presented)
        ++app->repeated_video_frames;
    else {
        ++app->unique_video_frames;
        app->video.presented = true;
    }
}

/* One frame: inputs, then (if the runtime wants pictures) the video, the
 * autofocus measurement, the panels and the overlay in one command
 * buffer, and the layers. */
static void render_frame(struct app *app)
{
    if (app->overlay.render_models_dirty) {
        app->overlay.render_models_dirty = false;
        render_models_load(&app->overlay.render_models, app->xr.instance,
                           app->xr.session, app->input.hands);
        upload_render_models(app);
    }
    struct frame f = {.state = {.type = XR_TYPE_FRAME_STATE}};
    XrFrameWaitInfo wait = {.type = XR_TYPE_FRAME_WAIT_INFO};
    double waiting = monotonic_seconds();
    need_xr("xrWaitFrame", xrWaitFrame(app->xr.session, &wait, &f.state));
    f.frame_started = monotonic_seconds();
    timing_frame(app, &f.state, (f.frame_started - waiting) * 1e3);
    XrFrameBeginInfo begin = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    need_xr("xrBeginFrame", xrBeginFrame(app->xr.session, &begin));
    ++app->xr_frames;
    app->xr.should_render = f.state.shouldRender;
    update_home(app, f.state.predictedDisplayTime);
    if (app->playing) {
        sync_player_input(app, f.state.predictedDisplayTime);
        sync_player_panel(app);
    } else {
        sync_browser_input(app, f.state.predictedDisplayTime);
        if (app->browser.dirty) {
            double drawing = monotonic_seconds();
            upload_browser(app);
            timing_ui(app, drawing);
        }
    }
    f.inputs_done = monotonic_seconds();
    if (!f.state.shouldRender) {
        XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = f.state.predictedDisplayTime,
            .environmentBlendMode = environment_blend_mode(app)};
        need_xr("xrEndFrame(empty)", xrEndFrame(app->xr.session, &end));
        return;
    }
    /* Without a decoded frame (browser, start, seek preroll) only the clear
     * colour and the panel are shown, so the headset never falls back to the
     * Steam environment. */
    if (app->playing && app->video.current >= 0)
        f.video = &app->video.buffers[app->video.current];
    else if (app->playing && app->video.has_held)
        f.video = &app->video.held;
    locate_views(app, &f);
    place_video(app, &f);
    plan_measurement(app, &f);
    write_view_uniforms(app, &f);
    wait_render_fence(app, &f);
    collect_measurement(app);
    record_video_start(app, &f);
    record_measurement(app, &f);
    place_panels(app, &f);
    record_video_passes(app, &f);
    record_frame_overlay(app, &f);
    submit_frame(app, &f);
    end_frame(app, &f);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) {
        fprintf(stderr, "usage: %s UI_FD\n", argv[0]);
        return 2;
    }
    struct app app = {0};
    app.ui_fd = (int)parse_u64(argv[1], "UI fd");
    app.link.fd = -1;
    app.video.buffer_count = VIDEO_BUFFER_COUNT;
    for (uint32_t i = 0; i < NV12_IPC_MAX_BUFFERS; ++i)
        app.video.buffers[i].dma_buf_fd = -1;
    app.video.held.dma_buf_fd = -1;
    app.video.current = -1;
    app.link.player_state = NV12_PLAYER_PLAYING;
    app.ui_visible = true;
    app.ui_dirty = true;
    app.panel_y = -0.35f;
    app.panel_z = -1.3f;
    app.grab_hand = -1;
    app.scrub_hand = -1;
    app.bar_u = -1.0f;
    app.controls.learn_action = -1;
    app.home_pending = true;
    app.settings_tab = PLAYER_UI_TAB_PLAYBACK;
    app.awaiting_frame = true;
    char language[16];
    settings_get_string("language", language, sizeof(language), "auto");
    app.system_language = i18n_detect();
    printf("language=%s choice=%s system=%s\n",
           i18n_languages[i18n_select(language)]->code, language,
           i18n_languages[app.system_language]->code);
    updater_init(&app.updater);
    app.update_auto = settings_get_bool("update_check", true);
    char browser_error[256] = {0};
    if (browser_ui_open(&app.browser, "root:", NULL, browser_error,
                        sizeof(browser_error)) < 0) {
        fprintf(stderr, "browser_error=%s\n", browser_error);
        return 2;
    }
    app.trickplay = trickplay_create();
    load_preview_settings(&app);
    load_adjust(&app, true);
    if (app.update_auto)
        updater_check(&app.updater);
    refresh_update_status(&app);
    initialize_xr_vulkan(&app);
    XrGraphicsBindingVulkan2KHR binding = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = app.gpu.instance, .physicalDevice = app.gpu.physical, .device = app.gpu.device, .queueFamilyIndex = app.gpu.queue_family, .queueIndex = 0};
    XrSessionCreateInfo session_info = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = app.xr.system};
    need_xr("xrCreateSession", xrCreateSession(app.xr.instance, &session_info, &app.xr.session));
    initialize_actions(&app);
    /* SteamVR on Frame requires READY/xrBeginSession before it permits a
     * reference space or swapchain to be created. */
    struct timespec pause = {.tv_nsec = 10000000};
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!app.xr.running) {
        poll_events(&app);
        clock_gettime(CLOCK_MONOTONIC, &now);
        need(now.tv_sec - started.tv_sec < 10,
             "OpenXR session did not enter READY within ten seconds");
        if (!app.xr.running) nanosleep(&pause, NULL);
    }
    XrReferenceSpaceCreateInfo space_info = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace = {{0,0,0,1},{0,0,0}}};
    need_xr("xrCreateReferenceSpace", xrCreateReferenceSpace(app.xr.session, &space_info, &app.xr.space));
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    need_xr("xrCreateReferenceSpace(VIEW)",
            xrCreateReferenceSpace(app.xr.session, &space_info,
                                   &app.xr.view_space));
    initialize_swapchain(&app);
    app.panels.panels_in_eye_images = panels_in_eye_images();
    create_ui_swapchain(&app);
    create_video_sampler(&app);
    create_render_resources(&app);
    create_ui_resources(&app);
    start_panel_thread(&app);
    create_browser_texture(&app);
    create_overlay(&app);
    create_panel_pipeline(&app);
    create_pointer_texture(&app);
    create_gaze_markers(&app);
    printf("host_ready\n");
    while (!app.quit) {
        handle_control(&app);
        if (app.playing) {
            receive_frame_messages(&app);
            struct ui_image *strip[TRICKPLAY_STRIP];
            if (trickplay_poll(app.trickplay, strip)) {
                for (int i = 0; i < TRICKPLAY_STRIP; ++i) {
                    ui_image_free(app.preview[i]);
                    app.preview[i] = strip[i];
                }
                if (app.preview_visible)
                    app.preview_redraw_pending = true; /* update_preview */
            }
            if (trickplay_details(app.trickplay, &app.details)) {
                app.details_valid = true;
                app.ui_dirty = true;
            }
        }
        browser_ui_update(&app.browser);
        poll_updater(&app);
        poll_events(&app);
        hide_dashboard_at_startup(&app);
        if (app.quit)
            break;
        if (app.xr.running) render_frame(&app); else nanosleep(&pause, NULL);
        if (app.playing && app.link.eos_received)
            end_playback(&app);
    }
    if (app.xr_exit)
        send_control(&app, PLAYER_UI_EXIT_REQUESTED, NULL, 0);
    if (app.playing)
        end_playback(&app);
    updater_close(&app.updater);
    browser_ui_close(&app.browser);
    stop_panel_thread(&app);
    player_ui_close(&app.panels.player_ui);
    trickplay_destroy(app.trickplay);
    for (int i = 0; i < TRICKPLAY_STRIP; ++i)
        ui_image_free(app.preview[i]);
    return 0;
}
