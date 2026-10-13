#define _POSIX_C_SOURCE 200809L
#include "updater.h"

#include "i18n.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SCRIPT "./stereonaut-update.sh"

/* What stereonaut-update.sh reports after "error". */
static const char *const script_errors[] = {
    N_("no connection to the server"),
    N_("no release found"),
    N_("no version given"),
    N_("invalid version"),
    N_("no release key installed"),
    N_("ssh-keygen is missing"),
    N_("download failed"),
    N_("the release has no signature"),
    N_("invalid signature"),
    N_("damaged package"),
    N_("installation failed"),
};

void updater_init(struct updater *updater)
{
    *updater = (struct updater){.pid = -1, .fd = -1};
    updater->usable = access(SCRIPT, X_OK) == 0 &&
                      access("release-signers", R_OK) == 0;
    FILE *manifest = fopen("MANIFEST", "r");
    char line[256];
    while (manifest && fgets(line, sizeof(line), manifest)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "STEREONAUT_VERSION=", 19) == 0)
            snprintf(updater->installed, sizeof(updater->installed),
                     "%.63s", line + 19);
    }
    if (manifest)
        fclose(manifest);
    printf("updater usable=%d installed=%s\n", updater->usable,
           updater->installed[0] ? updater->installed : "source");
}

static bool start(struct updater *updater, const char *command,
                  const char *argument)
{
    if (!updater->usable || updater->pid > 0)
        return false;
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0)
        return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        /* Neither the XR runtime's nor the launcher's descriptors. */
        for (int fd = 3; fd < 1024; ++fd)
            close(fd);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", SCRIPT, command, argument, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    fcntl(pipe_fds[0], F_SETFL, fcntl(pipe_fds[0], F_GETFL) | O_NONBLOCK);
    updater->pid = pid;
    updater->fd = pipe_fds[0];
    updater->line_used = 0;
    updater->error[0] = '\0';
    printf("updater %s %s\n", command, argument ? argument : "");
    return true;
}

bool updater_check(struct updater *updater)
{
    if (!start(updater, "check", NULL))
        return false;
    updater->state = UPDATER_CHECKING;
    return true;
}

bool updater_install(struct updater *updater)
{
    if (!updater->latest[0] || !start(updater, "install", updater->latest))
        return false;
    updater->state = UPDATER_INSTALLING;
    updater->phase = UPDATER_PHASE_DOWNLOAD;
    return true;
}

static void handle_line(struct updater *updater, const char *line)
{
    printf("updater_report %s\n", line);
    if (strncmp(line, "latest ", 7) == 0) {
        snprintf(updater->latest, sizeof(updater->latest), "%s", line + 7);
        updater->checked = true;
    } else if (strncmp(line, "error ", 6) == 0) {
        snprintf(updater->error, sizeof(updater->error), "%s", line + 6);
    } else if (strcmp(line, "phase verify") == 0) {
        updater->phase = UPDATER_PHASE_VERIFY;
    } else if (strcmp(line, "phase unpack") == 0) {
        updater->phase = UPDATER_PHASE_UNPACK;
    } else if (strcmp(line, "phase install") == 0) {
        updater->phase = UPDATER_PHASE_INSTALL;
    } else if (strcmp(line, "done") == 0) {
        updater->state = UPDATER_DONE;
    }
}

bool updater_poll(struct updater *updater)
{
    if (updater->fd < 0)
        return false;
    bool changed = false;
    char buffer[512];
    for (;;) {
        ssize_t count = read(updater->fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return changed; /* nothing more for now */
        if (count == 0)
            break;          /* the script ended */
        for (ssize_t i = 0; i < count; ++i) {
            if (buffer[i] != '\n') {
                if (updater->line_used + 1 < sizeof(updater->line))
                    updater->line[updater->line_used++] = buffer[i];
                continue;
            }
            updater->line[updater->line_used] = '\0';
            updater->line_used = 0;
            handle_line(updater, updater->line);
            changed = true;
        }
    }
    close(updater->fd);
    updater->fd = -1;
    int status = 0;
    while (waitpid(updater->pid, &status, 0) < 0 && errno == EINTR) {
    }
    updater->pid = -1;
    bool succeeded = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("updater_exit status=%d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    if (updater->state == UPDATER_INSTALLING || !succeeded) {
        if (updater->state != UPDATER_DONE) {
            updater->state = UPDATER_FAILED;
            if (!updater->error[0])
                snprintf(updater->error, sizeof(updater->error), "%s",
                         "installation failed");
        }
    } else if (updater->state == UPDATER_CHECKING) {
        updater->state = UPDATER_IDLE;
    }
    return true;
}

bool updater_available(const struct updater *updater)
{
    return updater->installed[0] && updater->latest[0] &&
           strcmp(updater->installed, updater->latest) != 0;
}

const char *updater_error(const struct updater *updater)
{
    for (size_t i = 0; i < sizeof(script_errors) / sizeof(script_errors[0]);
         ++i)
        if (strcmp(updater->error, script_errors[i]) == 0)
            return tr(script_errors[i]);
    return updater->error;
}

void updater_close(struct updater *updater)
{
    if (updater->pid > 0) {
        /* A running installation finishes on its own; only a check is
         * stopped. */
        if (updater->state == UPDATER_CHECKING)
            kill(updater->pid, SIGTERM);
        updater->pid = -1;
    }
    if (updater->fd >= 0)
        close(updater->fd);
    updater->fd = -1;
}
