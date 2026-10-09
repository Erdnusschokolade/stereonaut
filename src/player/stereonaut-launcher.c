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

static int wait_child(pid_t child, const char *name)
{
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s exited abnormally\n", name);
        return -1;
    }
    return 0;
}

static int run_browser(const char *directory, struct player_ui_result *result)
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

static int run_playback(const char *path)
{
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
    return wait_child(child, "stereonaut-session");
}

static void selected_parent(const char *path, char *directory, size_t size)
{
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
                              ? (argc == 3 ? argv[2] : getenv("HOME"))
                              : (argc == 2 ? argv[1] : getenv("HOME"));
    if (!initial || !initial[0])
        initial = "/";
    char directory[PLAYER_UI_PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", initial);
    if (browser_only) {
        struct player_ui_result result = {0};
        if (run_browser(directory, &result) < 0)
            return 1;
        printf("browser_result type=%u path=%s\n", result.type, result.path);
        return 0;
    }
    for (;;) {
        struct player_ui_result result = {0};
        if (run_browser(directory, &result) < 0)
            return 1;
        if (result.type == PLAYER_UI_EXIT_REQUESTED)
            return 0;
        if (result.type != PLAYER_UI_FILE_SELECTED || !result.path[0]) {
            fprintf(stderr, "browser returned unknown action\n");
            return 1;
        }
        selected_parent(result.path, directory, sizeof(directory));
        printf("selected_file=%s\n", result.path);
        if (run_playback(result.path) < 0)
            fprintf(stderr, "playback failed; returning to browser\n");
    }
}
