#ifndef STEREONAUT_UPDATER_H
#define STEREONAUT_UPDATER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Updates from the release page: runs stereonaut-update.sh (installed next
 * to the renderer, which runs in its install directory) in the background
 * and collects what it reports. The installed version comes from the
 * MANIFEST of a release package; a build from the sources has none. */

enum updater_state {
    UPDATER_IDLE,
    UPDATER_CHECKING,
    UPDATER_INSTALLING,
    UPDATER_DONE,   /* installed: the host restarts */
    UPDATER_FAILED, /* see error */
};

enum updater_phase {
    UPDATER_PHASE_DOWNLOAD,
    UPDATER_PHASE_VERIFY,
    UPDATER_PHASE_UNPACK,
    UPDATER_PHASE_INSTALL,
};

struct updater {
    enum updater_state state;
    enum updater_phase phase;
    bool usable;        /* script and release keys installed */
    bool checked;       /* a check has answered */
    char installed[64]; /* "" = built from the sources */
    char latest[64];    /* "" = not known */
    char error[160];    /* English, see updater_error */
    pid_t pid;
    int fd;
    char line[256];
    size_t line_used;
};

void updater_init(struct updater *updater);
/* Start a check or the installation of `latest`; false if busy or not
 * usable. */
bool updater_check(struct updater *updater);
bool updater_install(struct updater *updater);
/* Reads what the script reported; true if the state changed. */
bool updater_poll(struct updater *updater);
/* A newer release than the installed one (only for release installs). */
bool updater_available(const struct updater *updater);
/* The last error in the language in effect. */
const char *updater_error(const struct updater *updater);
void updater_close(struct updater *updater);

#endif
