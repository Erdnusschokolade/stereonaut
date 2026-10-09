#ifndef STEREONAUT_PLAYER_UI_IPC_H
#define STEREONAUT_PLAYER_UI_IPC_H

#include <stdint.h>

#define PLAYER_UI_PATH_MAX 4096U

enum player_ui_result_type {
    PLAYER_UI_FILE_SELECTED = 1,
    PLAYER_UI_EXIT_REQUESTED = 2,
};

struct player_ui_result {
    uint32_t type;
    char path[PLAYER_UI_PATH_MAX];
};

#endif
