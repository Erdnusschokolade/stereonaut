#ifndef STEREONAUT_UI_CONTROL_H
#define STEREONAUT_UI_CONTROL_H

#include <stdbool.h>

#include "player-ui.h"
#include "xr-session.h"

/* What the viewer does with the panels: pointing, pressing, grabbing and
 * hiding the player panel, the seek bar and its preview, the popup's
 * settings and picture values, the settings page and the browser. Works on
 * the player state in struct app. */

struct app;

/* Global settings (settings.json) at startup. */
void load_preview_settings(struct app *app);
/* Global values at startup, per-title values when a title starts. */
void load_adjust(struct app *app, bool global);
/* Per-title values go to the title's entry in titles.json, tilt and head
 * motion to settings.json. */
void save_adjust(const struct app *app, int index);
/* A stepper of the popup (PLAYER_UI_ADJUST + ...). */
void change_adjust(struct app *app, int control);
/* Steps of 10 %; above 100 % the samples are amplified (may clip). */
void change_volume(struct app *app, int direction);
/* Status line of the updates tab and the dot on "Settings". */
void refresh_update_status(struct app *app);
/* What the player panel shows, and the settings page over the browser. */
struct player_ui_state player_state(const struct app *app);
struct player_ui_state settings_state(const struct app *app);
/* Per frame: what the controllers do with the player panel and the
 * video, or with the browser and the settings page. */
void sync_player_input(struct app *app, XrTime time);
void sync_browser_input(struct app *app, XrTime time);

#endif
