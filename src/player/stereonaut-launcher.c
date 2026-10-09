/* Starts the persistent OpenXR host (browser and video renderer in one
 * process) and one playback session per selected title. Keeping a single XR
 * session alive avoids the SteamVR dashboard popping up between screens. */
#define _POSIX_C_SOURCE 200809L
#include "player-ui-ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

static int ui_fd = -1;
static pid_t host_pid = -1;

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

/* Duplicate a CLOEXEC descriptor to one a child inherits across exec. */
static int inheritable(int fd)
{
    return fcntl(fd, F_DUPFD, 3);
}

static int start_host(void)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0) {
        perror("socketpair host");
        return -1;
    }
    int child_fd = inheritable(sockets[1]);
    if (child_fd < 0) {
        perror("duplicate host fd");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    char fd_text[32];
    snprintf(fd_text, sizeof(fd_text), "%d", child_fd);
    host_pid = fork();
    if (host_pid < 0) {
        perror("fork host");
        close(child_fd);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    if (host_pid == 0) {
        close(sockets[0]);
        close(sockets[1]);
        execl("./stereonaut-renderer", "stereonaut-renderer", fd_text,
              (char *)NULL);
        perror("exec stereonaut-renderer");
        _exit(127);
    }
    close(child_fd);
    close(sockets[1]);
    ui_fd = sockets[0];
    return 0;
}

static int send_command(uint32_t type, const char *path, const char *message,
                        int fd)
{
    struct player_ui_command command = {.type = type};
    if (path)
        snprintf(command.path, sizeof(command.path), "%s", path);
    if (message)
        snprintf(command.message, sizeof(command.message), "%s", message);
    struct iovec iov = {.iov_base = &command, .iov_len = sizeof(command)};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr header = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        header.msg_control = control;
        header.msg_controllen = sizeof(control);
        struct cmsghdr *item = CMSG_FIRSTHDR(&header);
        item->cmsg_level = SOL_SOCKET;
        item->cmsg_type = SCM_RIGHTS;
        item->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(item), &fd, sizeof(fd));
    }
    ssize_t written;
    do {
        written = sendmsg(ui_fd, &header, MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)sizeof(command)) {
        fprintf(stderr, "XR host is not reachable\n");
        return -1;
    }
    return 0;
}

/* Runs one session attached to the host. `extra` holds additional session
 * arguments (used by --play for bounded test runs). */
static int run_playback(const char *path, char *const *extra, int *exit_code)
{
    *exit_code = -1;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0) {
        perror("socketpair playback");
        return -1;
    }
    int session_fd = inheritable(sockets[0]);
    if (session_fd < 0) {
        perror("duplicate session fd");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    char renderer_text[48];
    snprintf(renderer_text, sizeof(renderer_text), "%d,0", session_fd);
    pid_t child = fork();
    if (child < 0) {
        perror("fork playback");
        close(session_fd);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    if (child == 0) {
        close(sockets[0]);
        close(sockets[1]);
        char *argv[32];
        size_t count = 0;
        argv[count++] = "stereonaut-session";
        argv[count++] = (char *)path;
        argv[count++] = "--renderer";
        argv[count++] = renderer_text;
        for (; extra && *extra && count + 1 < sizeof(argv) / sizeof(argv[0]);
             ++extra)
            argv[count++] = *extra;
        argv[count] = NULL;
        execv("./stereonaut-session", argv);
        perror("exec stereonaut-session");
        _exit(127);
    }
    close(session_fd);
    close(sockets[0]);
    int sent = send_command(PLAYER_UI_PLAY, NULL, NULL, sockets[1]);
    close(sockets[1]);
    if (sent < 0)
        kill(child, SIGTERM);
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

static void shutdown_host(void)
{
    if (ui_fd >= 0) {
        close(ui_fd);
        ui_fd = -1;
    }
    if (host_pid > 0) {
        int status;
        waitpid(host_pid, &status, 0);
        host_pid = -1;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    bool play_only = argc >= 3 && strcmp(argv[1], "--play") == 0;
    if ((!play_only && argc > 2) || (argc >= 2 && !play_only &&
                                      strncmp(argv[1], "--", 2) == 0)) {
        fprintf(stderr,
                "usage: %s [INITIAL_LOCATION]\n"
                "       %s --play PATH [SESSION_ARGS...]\n",
                argv[0], argv[0]);
        return 2;
    }
    if (start_host() < 0)
        return 1;
    if (play_only) {
        /* Bounded test run: attach one session, then leave. */
        if (send_command(PLAYER_UI_BROWSE, "root:", NULL, -1) < 0) {
            shutdown_host();
            return 1;
        }
        int exit_code = 0;
        int result = run_playback(argv[2], &argv[3], &exit_code);
        shutdown_host();
        return result < 0 ? (exit_code > 0 ? exit_code : 1) : 0;
    }
    const char *initial = argc == 2 ? argv[1] : "root:";
    if (!initial[0])
        initial = "root:";
    char directory[PLAYER_UI_PATH_MAX];
    char browser_message[PLAYER_UI_MESSAGE_MAX] = {0};
    snprintf(directory, sizeof(directory), "%s", initial);
    if (send_command(PLAYER_UI_BROWSE, directory, NULL, -1) < 0) {
        shutdown_host();
        return 1;
    }
    for (;;) {
        struct player_ui_result result = {0};
        ssize_t received;
        do {
            received = recv(ui_fd, &result, sizeof(result), 0);
        } while (received < 0 && errno == EINTR);
        if (received != (ssize_t)sizeof(result)) {
            fprintf(stderr, "XR host ended\n");
            shutdown_host();
            return 0;
        }
        if (result.type == PLAYER_UI_EXIT_REQUESTED) {
            shutdown_host();
            return 0;
        }
        if (result.type != PLAYER_UI_FILE_SELECTED || !result.path[0]) {
            fprintf(stderr, "XR host returned unknown action\n");
            shutdown_host();
            return 1;
        }
        selected_parent(result.path, directory, sizeof(directory));
        printf("selected_file=%s\n", result.path);
        int exit_code = 0;
        browser_message[0] = '\0';
        if (run_playback(result.path, NULL, &exit_code) < 0) {
            fprintf(stderr, "playback failed; returning to browser\n");
            if (exit_code == EXIT_SOURCE_FAILED)
                snprintf(browser_message, sizeof(browser_message),
                         "JELLYFIN ODER NETZWERK NICHT ERREICHBAR");
            else
                snprintf(browser_message, sizeof(browser_message),
                         "VIDEO- ODER AUDIOFORMAT WIRD DERZEIT NICHT UNTERSTUETZT");
        }
        if (send_command(PLAYER_UI_BROWSE, directory, browser_message,
                         -1) < 0) {
            shutdown_host();
            return 1;
        }
    }
}
