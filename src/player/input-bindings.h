#ifndef STEREONAUT_INPUT_BINDINGS_H
#define STEREONAUT_INPUT_BINDINGS_H

#include <stdbool.h>

#include "controller-bindings.h"

/* What the controller bindings (controller-bindings.h) do: stick
 * directions and buttons, alone or as "hold one and press another", turn
 * into actions, repeat while held, or are assigned to the action waiting
 * for its buttons. */

/* Seeking with the bindings (seek_binding): presses less than
 * SEEK_BURST_GAP apart form a burst. Up to its fourth press each one
 * seeks at once; from the fifth, or once a held input repeats, playback
 * pauses and only the seek bar (with the trickplay preview) moves until
 * the input rests for SEEK_SCRUB_SETTLE. Held, the steps grow. */
#define SEEK_STEP_US INT64_C(10000000)
#define SEEK_BURST_GAP 0.5
#define SEEK_BURST_SCRUB 5
#define SEEK_SCRUB_SETTLE 0.5

struct controls {
    /* Bindings (settings.json "binding_*") and their input state: what was
     * down last frame, whether a held button served as a modifier (then it
     * does not act on release), and the repeating action + 1 per input. */
    struct binding bindings[BINDING_ACTION_COUNT];
    int learn_action; /* action waiting for its buttons, or -1 */
    bool held[BINDING_INPUT_COUNT];
    bool combined[BINDING_INPUT_COUNT];
    int repeat[BINDING_INPUT_COUNT];
    double repeat_time[BINDING_INPUT_COUNT];
    bool grip_toggle[2]; /* grip pressed on empty space: show/hide on release */
};

struct app;

/* Stores a binding in settings.json. */
void save_binding(const struct app *app, int action);
/* One step of seeking with the bindings or the panel's -10/+10 buttons,
 * see SEEK_BURST_GAP. Targets are absolute and continue from a pending
 * one, so the session lands where the seek bar shows. */
void seek_binding(struct app *app, int direction, bool repeated);
/* Turns stick directions and buttons into actions; while an action waits
 * for its buttons, the next press is assigned to it instead. */
void process_bindings(struct app *app, const bool grip[2]);

#endif
