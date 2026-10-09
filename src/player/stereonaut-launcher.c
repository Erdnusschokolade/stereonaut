#define _POSIX_C_SOURCE 200809L
#include "player-ui-ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Exit codes of stereonaut-session. */
#define EXIT_UNSUPPORTED_FORMAT 3
#define EXIT_SOURCE_FAILED 4

static int wait_child_status(pid_t child, const char *name, int *exit_code)
{
    int status = 0;
    *exit_code = -1;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s exited abnormally\n", name);
        if (WIFEXITED(status))
            *exit_code = WEXITSTATUS(status);
        return -1;
    }
    *exit_code = 0;
    return 0;
}

static int wait_child(pid_t child, const char *name)
{
    int exit_code;
    return wait_child_status(child, name, &exit_code);
}

static int run_browser(const char *directory, const char *message,
                       struct player_ui_result *result)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0) {
        perror("socketpair browser");
        return -1;
    }
    int child_fd = fcntl(sockets[1], F_DUPFD, 3);
    if (child_fd < 0) {
        perror("duplicate browser IPC fd");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    char fd_text[32];
    snprintf(fd_text, sizeof(fd_text), "%d", child_fd);
    pid_t child = fork();
    if (child < 0) {
        perror("fork browser");
        close(child_fd);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    if (child == 0) {
        close(sockets[0]);
        close(sockets[1]);
        if (message && message[0])
            execl("./stereonaut-browser", "stereonaut-browser",
                  fd_text, directory, message, (char *)NULL);
        else
            execl("./stereonaut-browser", "stereonaut-browser",
                  fd_text, directory, (char *)NULL);
        perror("exec stereonaut-browser");
        _exit(127);
    }
    close(child_fd);
    close(sockets[1]);
    ssize_t received;
    do {
        received = recv(sockets[0], result, sizeof(*result), 0);
    } while (received < 0 && errno == EINTR);
    close(sockets[0]);
    int child_result = wait_child(child, "stereonaut-browser");
    if (received != (ssize_t)sizeof(*result)) {
        fprintf(stderr, "browser returned no valid selection\n");
        return -1;
    }
    return child_result;
}

static int run_playback(const char *path, int *exit_code)
{
    *exit_code = -1;
    pid_t child = fork();
    if (child < 0) {
        perror("fork playback");
        return -1;
    }
    if (child == 0) {
        execl("./stereonaut-session", "stereonaut-session", path,
              "--projection", "180", "--stereo", "sbs", (char *)NULL);
        perror("exec stereonaut-session");
        _exit(127);
    }
    return wait_child_status(child, "stereonaut-session", exit_code);
}

static void selected_parent(const char *path, char *directory, size_t size)
{
    if (strncmp(path, "jf:item:", 8) == 0) {
        /* jf:item:<id>:<folder> returns to the folder it was chosen in. */
        const char *folder = strchr(path + 8, ':');
        if (folder && folder[1])
            snprintf(directory, size, "jf:view:%s", folder + 1);
        else
            snprintf(directory, size, "jf:");
        return;
    }
    snprintf(directory, size, "%s", path);
    char *slash = strrchr(directory, '/');
    if (!slash || slash == directory)
        snprintf(directory, size, "/");
    else
        *slash = '\0';
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    bool browser_only = argc >= 2 &&
                        strcmp(argv[1], "--browser-only") == 0;
    if ((!browser_only && argc > 2) || (browser_only && argc > 3)) {
        fprintf(stderr,
                "usage: %s [INITIAL_DIRECTORY]\n"
                "       %s --browser-only [INITIAL_DIRECTORY]\n",
                argv[0], argv[0]);
        return 2;
    }
    const char *initial = browser_only
                              ? (argc == 3 ? argv[2] : "root:")
                              : (argc == 2 ? argv[1] : "root:");
    if (!initial || !initial[0])
        initial = "root:";
    char directory[PLAYER_UI_PATH_MAX];
    char browser_message[256] = {0};
    snprintf(directory, sizeof(directory), "%s", initial);
    if (browser_only) {
        struct player_ui_result result = {0};
        if (run_browser(directory, NULL, &result) < 0)
            return 1;
        printf("browser_result type=%u path=%s\n", result.type, result.path);
        return 0;
    }
    for (;;) {
        struct player_ui_result result = {0};
        if (run_browser(directory, browser_message, &result) < 0)
            return 1;
        browser_message[0] = '\0';
        if (result.type == PLAYER_UI_EXIT_REQUESTED)
            return 0;
        if (result.type != PLAYER_UI_FILE_SELECTED || !result.path[0]) {
            fprintf(stderr, "browser returned unknown action\n");
            return 1;
        }
        selected_parent(result.path, directory, sizeof(directory));
        printf("selected_file=%s\n", result.path);
        int exit_code = 0;
        if (run_playback(result.path, &exit_code) < 0) {
            fprintf(stderr, "playback failed; returning to browser\n");
            if (exit_code == EXIT_SOURCE_FAILED)
                snprintf(browser_message, sizeof(browser_message),
                         "JELLYFIN ODER NETZWERK NICHT ERREICHBAR");
            else
                snprintf(browser_message, sizeof(browser_message),
                         "VIDEO- ODER AUDIOFORMAT WIRD DERZEIT NICHT UNTERSTUETZT");
        }
    }
}
