#ifndef STEREONAUT_CONTROLLER_BINDINGS_H
#define STEREONAUT_CONTROLLER_BINDINGS_H

#include <stdbool.h>
#include <stddef.h>

/* Controller buttons of the player and the actions they trigger. A binding
 * is an input (stick direction or button), optionally with a modifier: a
 * button held while the input is pressed ("Griff + Stick →"). Bindings are
 * global (settings.json "binding_KEY"); trigger and grip keep their panel
 * functions, the grip is only available as a modifier. The renderer reads
 * the inputs, this file only knows names, defaults and matching. */

enum binding_input {
    /* Stick directions: hand * 4 + direction (left, right, up, down);
     * BINDING_ANY_STICK_* match either hand. */
    BINDING_LEFT_STICK_LEFT,
    BINDING_LEFT_STICK_RIGHT,
    BINDING_LEFT_STICK_UP,
    BINDING_LEFT_STICK_DOWN,
    BINDING_RIGHT_STICK_LEFT,
    BINDING_RIGHT_STICK_RIGHT,
    BINDING_RIGHT_STICK_UP,
    BINDING_RIGHT_STICK_DOWN,
    BINDING_ANY_STICK_LEFT,
    BINDING_ANY_STICK_RIGHT,
    BINDING_ANY_STICK_UP,
    BINDING_ANY_STICK_DOWN,
    /* Buttons (usable as modifiers). */
    BINDING_LEFT_STICK_CLICK,
    BINDING_RIGHT_STICK_CLICK,
    BINDING_LEFT_GRIP,
    BINDING_RIGHT_GRIP,
    BINDING_LEFT_BUMPER,
    BINDING_RIGHT_BUMPER,
    BINDING_DPAD_UP,
    BINDING_DPAD_DOWN,
    BINDING_DPAD_LEFT,
    BINDING_DPAD_RIGHT,
    BINDING_VIEW,
    BINDING_A,
    BINDING_B,
    BINDING_X,
    BINDING_Y,
    BINDING_MENU,
    BINDING_INPUT_COUNT,
};

#define BINDING_FIRST_BUTTON BINDING_LEFT_STICK_CLICK

enum binding_action {
    BINDING_SEEK_BACK,
    BINDING_SEEK_FORWARD,
    BINDING_VOLUME_DOWN,
    BINDING_VOLUME_UP,
    BINDING_PREVIOUS,
    BINDING_NEXT,
    BINDING_TILT_DOWN,
    BINDING_TILT_UP,
    BINDING_ZOOM_OUT,
    BINDING_ZOOM_IN,
    BINDING_PLAY_PAUSE,
    BINDING_ACTION_COUNT,
};

struct binding {
    int input;    /* enum binding_input, -1 = not bound */
    int modifier; /* button held at the same time, -1 = none */
};

struct binding_action_info {
    const char *label;
    const char *key;  /* settings.json "binding_" + key */
    bool repeat;      /* repeats while held */
    struct binding standard;
};

extern const struct binding_action_info
    binding_action_info[BINDING_ACTION_COUNT];

static inline bool binding_is_button(int input)
{
    return input >= BINDING_FIRST_BUTTON && input < BINDING_INPUT_COUNT;
}

/* Stick direction input of `hand` (0 left, 1 right) and `direction`
 * (0 left, 1 right, 2 up, 3 down). */
static inline int binding_stick(unsigned hand, int direction)
{
    return (int)hand * 4 + direction;
}

/* Settings value of a binding and back (0 = not bound). */
int binding_encode(struct binding binding);
struct binding binding_decode(int value);
/* Short text such as "R Griff + Stick →" or "nicht belegt". */
void binding_describe(struct binding binding, char *text, size_t size);
/* True if some binding uses `button` as its modifier. */
bool binding_is_modifier(const struct binding bindings[BINDING_ACTION_COUNT],
                         int button);
/* Action for pressing `input` (a hand's stick direction or a button) while
 * the buttons in `held` are down, or -1. A binding with a held modifier
 * wins over one without, the input's own hand over "either hand".
 * *modifier is the modifier that matched, or -1. */
int binding_match(const struct binding bindings[BINDING_ACTION_COUNT],
                  int input, const bool held[BINDING_INPUT_COUNT],
                  int *modifier);

#endif
