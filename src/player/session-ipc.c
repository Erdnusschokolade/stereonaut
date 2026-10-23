#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

void send_message(struct app *app, const struct nv12_ipc_message *message)
{
    ssize_t written;
    do {
        written = send(app->link.fd, message, sizeof(*message), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written < 0 && (errno == EPIPE || errno == ECONNRESET ||
                        errno == ENOTCONN)) {
        app->link.eos_received = true; /* the session is gone: end playback */
        return;
    }
    if (written < 0) { perror("send NV12 IPC response"); exit(2); }
    need((size_t)written == sizeof(*message), "short NV12 IPC response");
}

void send_ipc(struct app *app, uint32_t type, uint32_t index)
{
    struct nv12_ipc_message message = {.type = type, .index = index};
    send_message(app, &message);
}

void send_volume(struct app *app)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_VOLUME, .volume = app->volume,
    };
    send_message(app, &message);
}

/* `state`: NV12_PLAYER_PLAYING or _PAUSED after the seek, 0 keeps it. */
void send_seek_to(struct app *app, int64_t target_us, uint32_t state)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_ACTION, .index = NV12_ACTION_SEEK_TO,
        .position_us = target_us, .player_state = state,
        .frame_index = ++app->link.seek_serial,
    };
    send_message(app, &message);
}

void send_panel_state(struct app *app)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_PANEL,
        .panel_x = app->panel_x, .panel_y = app->panel_y,
        .panel_z = app->panel_z, .panel_yaw = app->panel_yaw,
        .panel_visible = app->ui_visible,
        .panel_valid = 1,
    };
    send_message(app, &message);
}

static void release_current_video(struct app *app)
{
    if (app->video.current < 0)
        return;
    if (!app->video.presented) {
        if (app->xr.should_render)
            app->pipeline_skipped_frames++;
        else
            app->hidden_video_frames++;
    }
    send_ipc(app, NV12_IPC_RELEASE, (uint32_t)app->video.current);
    app->released_frames++;
    app->video.current = -1;
}

void receive_frame_messages(struct app *app)
{
    if (video_import_pending(app, false))
        return;
    for (;;) {
        struct nv12_ipc_message message = {0};
        char control[CMSG_SPACE(sizeof(int) * NV12_IPC_MAX_BUFFERS)] = {0};
        struct iovec iov = {.iov_base = &message, .iov_len = sizeof(message)};
        struct msghdr received = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control),
        };
        ssize_t bytes = recvmsg(app->link.fd, &received, MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes == 0) {
            app->link.eos_received = true;
            return;
        }
        if (bytes < 0 && errno == ECONNRESET) {
            app->link.eos_received = true;
            return;
        }
        if (bytes < 0) { perror("recv NV12 IPC message"); exit(2); }
        need((size_t)bytes == sizeof(message), "short NV12 IPC message");
        if (message.type == NV12_IPC_FRAME) {
            need(message.index < app->video.buffer_count,
                 "NV12 IPC frame index is invalid");
            release_current_video(app);
            app->video.current = (int)message.index;
            app->video.presented = false;
            app->video.buffers[message.index].producer_wait_pending = true;
            if (app->awaiting_frame) {
                app->awaiting_frame = false;
                /* The picture of an earlier seek keeps showing the target
                 * of the last one (else the time jumps back and forth). */
                if (app->link.seek_landing == app->link.seek_serial &&
                    app->seek_pending) {
                    /* Until the next STATUS: the last one predates the
                     * seek. */
                    app->link.position_us = app->scrub_us;
                    app->seek_pending = false;
                }
                app->ui_dirty = true;
            }
            /* The held frame is superseded by the new stream. */
            discard_held_frame(app);
            app->received_frames++;
        } else if (message.type == NV12_IPC_FLUSH) {
            release_current_video(app);
            send_ipc(app, NV12_IPC_FLUSHED, 0);
        } else if (message.type == NV12_IPC_HOLD) {
            app->link.seek_landing = message.frame_index;
            hold_current_frame(app);
            send_ipc(app, NV12_IPC_HELD, 0);
        } else if (message.type == NV12_IPC_SEEKING) {
            app->link.seek_landing = message.frame_index;
            app->awaiting_frame = true;
            app->ui_dirty = true;
            printf("video_seeking\n");
        } else if (message.type == NV12_IPC_INIT) {
            reinitialize_video(app, &message, &received);
        } else if (message.type == NV12_IPC_FORMAT) {
            need(message.projection_mode <= NV12_PROJECTION_FISHEYE &&
                     message.stereo_layout <= NV12_STEREO_TB &&
                     message.format_source <= NV12_FORMAT_SOURCE_USER,
                 "invalid detected format");
            app->projection_mode = message.projection_mode;
            app->stereo_layout = message.stereo_layout;
            app->format_source = message.format_source;
            if (message.fisheye_fov >= 120 && message.fisheye_fov <= 240)
                app->fisheye_fov = message.fisheye_fov;
            app->ui_dirty = true;
            printf("player_format projection=%u stereo=%u source=%u\n",
                   app->projection_mode, app->stereo_layout,
                   app->format_source);
        } else if (message.type == NV12_IPC_EOS) {
            app->link.eos_received = true;
            app->link.title_finished = message.index == 1;
        } else if (message.type == NV12_IPC_STATUS) {
            /* Redraw only for a visible change: the shown second, the
             * seek bar knob (whole pixels) or the play state. */
            int64_t old_second = app->link.position_us / 1000000;
            int old_knob = app->link.duration_us > 0
                ? (int)(1000.0 * app->link.position_us / app->link.duration_us) : -1;
            bool changed = app->link.player_state != message.player_state ||
                           app->link.duration_us != message.duration_us;
            app->link.player_state = message.player_state;
            if (!app->seek_pending)
                app->link.position_us = message.position_us;
            app->link.duration_us = message.duration_us;
            int knob = app->link.duration_us > 0
                ? (int)(1000.0 * app->link.position_us / app->link.duration_us) : -1;
            if (changed || old_second != app->link.position_us / 1000000 ||
                old_knob != knob)
                app->ui_dirty = true;
        } else {
            need(false, "unknown NV12 IPC message");
        }
        /* The socket is the bounded FIFO. Consume at most one decoded frame
         * per XR iteration so a decoder burst cannot discard valid frames. */
        return;
    }
}
