/* Starts the persistent OpenXR host (browser and video renderer in one
 * process) and one playback session per selected title. Keeping a single XR
 * session alive avoids the SteamVR dashboard popping up between screens. */
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
#include "player-ui-ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Exit codes of stereonaut-session. */
#define EXIT_UNSUPPORTED_FORMAT 3
#define EXIT_SOURCE_FAILED 4
#define EXIT_BROKEN_SOURCE 5
#define EXIT_DECODER_BUSY 6

static int ui_fd = -1;
static pid_t host_pid = -1;
static bool host_failed; /* the XR host ended with an error or a signal */

static bool exit_after_playback; /* the host ended during playback */

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int child_status(int status, const char *name, int *exit_code)
{
    *exit_code = -1;
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
    int sent = send_command(PLAYER_UI_PLAY, path, NULL, sockets[1]);
    close(sockets[1]);
    if (sent < 0)
        kill(child, SIGTERM);
    /* While the session runs, the host may ask to stop it (back pressed
     * during loading) or report that the XR session ended. */
    double stop_sent = 0.0;
    int watch_fd = ui_fd; /* -1 once the host has closed its end */
    for (;;) {
        int status = 0;
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child)
            return child_status(status, "stereonaut-session", exit_code);
        if (done < 0 && errno != EINTR) {
            perror("waitpid");
            return -1;
        }
        struct pollfd pollfd = {.fd = watch_fd, .events = POLLIN};
        if (poll(&pollfd, 1, 100) > 0) {
            struct player_ui_result result = {0};
            ssize_t received = recv(watch_fd, &result, sizeof(result),
                                    MSG_DONTWAIT);
            if (received == (ssize_t)sizeof(result) &&
                result.type == PLAYER_UI_EXIT_REQUESTED)
                exit_after_playback = true;
            if (received == 0 ||
                (received == (ssize_t)sizeof(result) &&
                 (result.type == PLAYER_UI_STOP_REQUESTED ||
                  result.type == PLAYER_UI_EXIT_REQUESTED))) {
                if (!stop_sent) {
                    printf("playback_stop_requested\n");
                    kill(child, SIGTERM);
                    stop_sent = monotonic_seconds();
                }
            }
            if (received == 0) {
                exit_after_playback = true;
                watch_fd = -1;
            }
        }
        if (stop_sent && monotonic_seconds() - stop_sent > 5.0) {
            fprintf(stderr, "stereonaut-session ignored SIGTERM\n");
            kill(child, SIGKILL);
            stop_sent = monotonic_seconds() + 3600.0;
        }
    }
}

static void selected_parent(const char *path, char *directory, size_t size)
{
    if (strncmp(path, "jf:item:", 8) == 0) {
        /* jf:item:<id>:<folder> returns to the folder it was chosen in;
         * titles from the start page ("continue watching") have none. */
        const char *folder = strchr(path + 8, ':');
        if (folder && folder[1] == '?') /* search results */
            snprintf(directory, size, "jf:search:%s", folder + 2);
        else if (folder && folder[1])
            snprintf(directory, size, "jf:view:%s", folder + 1);
        else
            snprintf(directory, size, "root:");
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
        if (waitpid(host_pid, &status, 0) == host_pid &&
            (WIFSIGNALED(status) ||
             (WIFEXITED(status) && WEXITSTATUS(status) != 0))) {
            host_failed = true;
            fprintf(stderr, "XR host failed (status %d)\n", status);
        }
        host_pid = -1;
    }
}

/* Runs the start script (STEREONAUT_START_SCRIPT, which it sets, else
 * ~/.local/bin/stereonaut) again in this process, which SteamVR keeps as
 * the running app, with `variable` set: after an update
 * (STEREONAUT_RESTART) or when the XR host failed (STEREONAUT_HOST_FAILED:
 * the script offers a newer release, as a system update may have broken
 * the old one). */
static int run_start_script(const char *variable)
{
    char script[PLAYER_UI_PATH_MAX];
    const char *home = getenv("HOME");
    const char *own = getenv("STEREONAUT_START_SCRIPT");
    if (own && own[0])
        snprintf(script, sizeof(script), "%s", own);
    else
        snprintf(script, sizeof(script), "%s/.local/bin/stereonaut",
                 home ? home : "");
    printf("start_script=%s %s\n", script, variable);
    fflush(stdout);
    fflush(stderr);
    setenv(variable, "1", 1);
    execl(script, script, (char *)NULL);
    perror("start script");
    return 1;
}

/* Ends the host; if it failed, hands over to the start script. */
static int finish(int code)
{
    shutdown_host();
    return host_failed ? run_start_script("STEREONAUT_HOST_FAILED") : code;
}

/* "--language": the code of the interface language (settings.json, else
 * the system's), for the dialogs of the start script. Only the C library
 * is needed, so this answers when a SteamOS update broke the rest. */
static int print_language(void)
{
    char path[4200], choice[32] = "auto";
    const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (base && base[0])
        snprintf(path, sizeof(path), "%s/stereonaut/settings.json", base);
    else
        snprintf(path, sizeof(path), "%s/.config/stereonaut/settings.json",
                 home ? home : "");
    FILE *file = fopen(path, "r");
    if (file) {
        char text[16384];
        size_t length = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[length] = '\0';
        const char *key = strstr(text, "\"language\"");
        const char *colon = key ? strchr(key + 10, ':') : NULL;
        const char *quote = colon ? strchr(colon, '"') : NULL;
        size_t size = quote ? strcspn(quote + 1, "\"") : 0;
        if (quote && size < sizeof(choice)) {
            memcpy(choice, quote + 1, size);
            choice[size] = '\0';
        }
    }
    printf("%s\n", i18n_languages[i18n_select(choice)]->code);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "--language") == 0)
        return print_language();
    bool play_only = argc >= 3 && strcmp(argv[1], "--play") == 0;
    if ((!play_only && argc > 2) || (argc >= 2 && !play_only &&
                                      strncmp(argv[1], "--", 2) == 0)) {
        fprintf(stderr,
                "usage: %s [INITIAL_LOCATION]\n"
                "       %s --play PATH [SESSION_ARGS...]\n"
                "       %s --language\n",
                argv[0], argv[0], argv[0]);
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
    if (send_command(PLAYER_UI_BROWSE, directory, NULL, -1) < 0)
        return finish(1);
    for (;;) {
        struct player_ui_result result = {0};
        ssize_t received;
        do {
            received = recv(ui_fd, &result, sizeof(result), 0);
        } while (received < 0 && errno == EINTR);
        if (received != (ssize_t)sizeof(result)) {
            fprintf(stderr, "XR host ended\n");
            return finish(0);
        }
        if (result.type == PLAYER_UI_EXIT_REQUESTED) {
            shutdown_host();
            return 0;
        }
        if (result.type == PLAYER_UI_RESTART_REQUESTED) {
            /* The script checks the libraries and starts the new
             * launcher. */
            shutdown_host();
            return run_start_script("STEREONAUT_RESTART");
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
        char start_text[32];
        char *start_arguments[] = {"--start-us", start_text, NULL};
        snprintf(start_text, sizeof(start_text), "%lld",
                 (long long)result.start_us);
        if (result.start_us > 0)
            printf("selected_start_us=%s\n", start_text);
        int played = run_playback(result.path,
                                  result.start_us > 0 ? start_arguments
                                                      : NULL,
                                  &exit_code);
        if (exit_after_playback)
            return finish(0);
        if (played < 0) {
            fprintf(stderr, "playback failed; returning to browser\n");
            if (exit_code == EXIT_BROKEN_SOURCE)
                snprintf(browser_message, sizeof(browser_message), "%s",
                         N_("The video file on the server is broken"));
            else if (exit_code == EXIT_DECODER_BUSY)
                snprintf(browser_message, sizeof(browser_message), "%s",
                         N_("Another program is using the video decoder "
                            "(e.g. a video in Steam)"));
            else if (exit_code == EXIT_SOURCE_FAILED)
                snprintf(browser_message, sizeof(browser_message), "%s",
                         N_("Cannot reach Jellyfin or the network"));
            else
                snprintf(browser_message, sizeof(browser_message), "%s",
                         N_("This video or audio format is not supported "
                            "yet"));
        }
        if (send_command(PLAYER_UI_BROWSE, directory, browser_message,
                         -1) < 0)
            return finish(1);
    }
}
