#ifndef STEREONAUT_PLAYBACK_REPORT_H
#define STEREONAUT_PLAYBACK_REPORT_H

#include <stdbool.h>
#include <stdint.h>

/* Reports a Jellyfin playback to the server (start, progress every ten
 * seconds and on pause changes, stop) from a worker thread, so a slow
 * server never stalls decoding. A seek re-executes the session; the new
 * process is created with `resumed` and continues the same playback with
 * progress reports instead of a second start. */

struct jf_config;
struct jf_playback;
struct playback_report;

struct playback_report *playback_report_create(
    const struct jf_config *config, const struct jf_playback *playback,
    bool resumed);
/* Latest state; nothing is reported before `playing` (first frame and
 * audio running) becomes true. */
void playback_report_update(struct playback_report *report,
                            int64_t position_us, bool paused, bool playing);
/* Ends the worker. With `stopped` the playback is reported as finished at
 * the last position (blocks for at most a few seconds); without it (seek
 * restart) the next process continues it. */
void playback_report_finish(struct playback_report *report, bool stopped);

#endif
