#define _POSIX_C_SOURCE 200809L
#include "playback-report.h"

#include "jellyfin.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PROGRESS_INTERVAL_S 10.0

struct playback_report {
    struct jf_config config;
    struct jf_playback playback;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    bool resumed;   /* an earlier process already reported the start */
    bool announced; /* this process reported start or continuation */
    bool quit;
    int64_t position_us;
    bool paused, playing;
};

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void send_report(struct playback_report *report, enum jf_report kind,
                        int64_t position_us, bool paused, const char *event)
{
    static const char *const names[] = {"start", "progress", "stopped"};
    int result = jf_report_playback(&report->config, &report->playback, kind,
                                    position_us, paused, event);
    printf("jellyfin_report=%s event=%s position_us=%lld result=%s\n",
           names[kind], event ? event : "-", (long long)position_us,
           result == 0 ? "ok" : "failed");
}

static void *worker(void *argument)
{
    struct playback_report *report = argument;
    double last_sent = 0.0;
    bool reported_paused = false;
    pthread_mutex_lock(&report->mutex);
    while (!report->quit) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        /* Wakes early on pause changes; spurious wakeups are harmless. */
        pthread_cond_timedwait(&report->wake, &report->mutex, &until);
        if (report->quit || !report->playing)
            continue;
        int64_t position = report->position_us;
        bool paused = report->paused;
        double now = monotonic_seconds();
        enum jf_report kind = JF_REPORT_PROGRESS;
        const char *event = NULL;
        if (!report->announced) {
            if (!report->resumed)
                kind = JF_REPORT_START;
            else
                event = "timeupdate";
            report->announced = true;
        } else if (paused != reported_paused) {
            event = paused ? "pause" : "unpause";
        } else if (now - last_sent >= PROGRESS_INTERVAL_S) {
            event = "timeupdate";
        } else {
            continue;
        }
        reported_paused = paused;
        last_sent = now;
        pthread_mutex_unlock(&report->mutex);
        send_report(report, kind, position, paused, event);
        pthread_mutex_lock(&report->mutex);
    }
    pthread_mutex_unlock(&report->mutex);
    return NULL;
}

struct playback_report *playback_report_create(
    const struct jf_config *config, const struct jf_playback *playback,
    bool resumed)
{
    struct playback_report *report = calloc(1, sizeof(*report));
    if (!report)
        return NULL;
    report->config = *config;
    report->playback = *playback;
    report->resumed = resumed;
    pthread_mutex_init(&report->mutex, NULL);
    pthread_cond_init(&report->wake, NULL);
    if (pthread_create(&report->thread, NULL, worker, report) != 0) {
        pthread_cond_destroy(&report->wake);
        pthread_mutex_destroy(&report->mutex);
        free(report);
        return NULL;
    }
    return report;
}

void playback_report_update(struct playback_report *report,
                            int64_t position_us, bool paused, bool playing)
{
    if (!report)
        return;
    pthread_mutex_lock(&report->mutex);
    bool changed = paused != report->paused || playing != report->playing;
    report->position_us = position_us;
    report->paused = paused;
    report->playing = playing;
    if (changed)
        pthread_cond_signal(&report->wake);
    pthread_mutex_unlock(&report->mutex);
}

void playback_report_finish(struct playback_report *report, bool stopped)
{
    if (!report)
        return;
    pthread_mutex_lock(&report->mutex);
    report->quit = true;
    pthread_cond_signal(&report->wake);
    pthread_mutex_unlock(&report->mutex);
    pthread_join(report->thread, NULL);
    if (stopped && (report->announced || report->resumed))
        send_report(report, JF_REPORT_STOPPED, report->position_us,
                    report->paused, NULL);
    pthread_cond_destroy(&report->wake);
    pthread_mutex_destroy(&report->mutex);
    free(report);
}
