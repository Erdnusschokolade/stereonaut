#ifndef STEREONAUT_PLAYER_UI_IPC_H
#define STEREONAUT_PLAYER_UI_IPC_H

#include <stdint.h>

#define PLAYER_UI_PATH_MAX 4096U
#define PLAYER_UI_MESSAGE_MAX 256U

/* Messages from the XR host to the launcher. */
enum player_ui_result_type {
    PLAYER_UI_FILE_SELECTED = 1,
    PLAYER_UI_EXIT_REQUESTED = 2,
    /* "Back" while the title is still loading: the session cannot answer
     * then, so the launcher stops it. */
    PLAYER_UI_STOP_REQUESTED = 3,
};

struct player_ui_result {
    uint32_t type;
    /* FILE_SELECTED: start position (0 = from the beginning). */
    int64_t start_us;
    char path[PLAYER_UI_PATH_MAX];
};

/* Commands from the launcher to the XR host. */
enum player_ui_command_type {
    /* Show the browser at `path` with an optional status `message`. */
    PLAYER_UI_BROWSE = 1,
    /* Attach a playback session: the NV12 socket travels as SCM_RIGHTS,
     * `path` names the title (as in FILE_SELECTED). */
    PLAYER_UI_PLAY = 2,
};

struct player_ui_command {
    uint32_t type;
    char path[PLAYER_UI_PATH_MAX];
    char message[PLAYER_UI_MESSAGE_MAX];
};

#endif
