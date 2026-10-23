#ifndef STEREONAUT_SESSION_IPC_H
#define STEREONAUT_SESSION_IPC_H

#include <stdbool.h>
#include <stdint.h>

#include "nv12-playback-ipc.h"

/* The NV12 socket to the playing session (nv12-playback-ipc.h): frames,
 * status and format from the session, actions, seeks, volume, view and
 * panel state to it. */

struct session_link {
    int fd; /* -1 without a session */
    bool eos_received;
    bool title_finished; /* EOS after the last frame, not "back" */
    uint64_t seek_serial;  /* number of the last SEEK_TO sent */
    uint64_t seek_landing; /* the one the session carries out (HOLD) */
    /* Last STATUS. */
    uint32_t player_state;
    int64_t position_us;
    int64_t duration_us;
};

struct app;

void send_message(struct app *app, const struct nv12_ipc_message *message);
void send_ipc(struct app *app, uint32_t type, uint32_t index);
void send_volume(struct app *app);
/* `state`: NV12_PLAYER_PLAYING or _PAUSED after the seek, 0 keeps it. */
void send_seek_to(struct app *app, int64_t target_us, uint32_t state);
void send_panel_state(struct app *app);
/* At most one message per frame (the socket is the bounded FIFO). */
void receive_frame_messages(struct app *app);

#endif
