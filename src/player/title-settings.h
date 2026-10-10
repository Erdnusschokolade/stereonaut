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
    uint32_t video_levels;    /* enum nv12_video_levels */
    /* enum nv12_format_source of projection and stereo layout; entries
     * written before detection existed count as the viewer's choice. */
    uint32_t format_source;
    uint32_t fisheye_fov;     /* degrees, for NV12_PROJECTION_FISHEYE */
};

#define TITLE_KEY_SIZE 4200

/* Default for titles without a stored entry: 180 degrees, side by side,
 * format source DEFAULT (to be detected). */
void title_settings_defaults(struct title_settings *settings);

/* Stable key for "jf:item:ID[:FOLDER]" specs, URLs and file paths. */
void title_settings_key(const char *input, char *key, size_t key_size);

/* Returns 1 and fills `settings` if an entry exists, 0 if not (settings keep
 * their value), -1 if the store is unreadable. */
int title_settings_load(const char *key, struct title_settings *settings);
int title_settings_save(const char *key, const struct title_settings *settings);

/* Numeric values under entry[group][names[i]] (NULL names are skipped):
 * load sets the values present and returns 1 if the group exists, 0 if
 * not, -1 if the store is unreadable; save replaces the group. */
int title_settings_load_numbers(const char *key, const char *group,
                                const char *const *names, float *values,
                                size_t count);
int title_settings_save_numbers(const char *key, const char *group,
                                const char *const *names,
                                const float *values, size_t count);

#endif
