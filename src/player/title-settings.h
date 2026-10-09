#ifndef STEREONAUT_TITLE_SETTINGS_H
#define STEREONAUT_TITLE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-title viewing settings, kept in $XDG_CONFIG_HOME/stereonaut/titles.json.
 * Titles are identified by a key derived from the player input: the item id
 * for Jellyfin, the resolved path for local files. Unknown fields of an entry
 * are preserved on save so later settings can share the entry. */

struct title_settings {
    uint32_t projection_mode; /* enum nv12_projection_mode */
    uint32_t stereo_layout;   /* enum nv12_stereo_layout */
    bool swap_eyes;
};

#define TITLE_KEY_SIZE 4200

/* Default for titles without a stored entry: 180 degrees, side by side. */
void title_settings_defaults(struct title_settings *settings);

/* Stable key for "jf:item:ID[:FOLDER]" specs, URLs and file paths. */
void title_settings_key(const char *input, char *key, size_t key_size);

/* Returns 1 and fills `settings` if an entry exists, 0 if not (settings keep
 * their value), -1 if the store is unreadable. */
int title_settings_load(const char *key, struct title_settings *settings);
int title_settings_save(const char *key, const struct title_settings *settings);

#endif
