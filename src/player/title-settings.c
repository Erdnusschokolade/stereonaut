#define _XOPEN_SOURCE 700
#include "title-settings.h"

#include "../../tools/openxr-playback/nv12-playback-ipc.h"

#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const projection_names[] = {"flat", "180", "360"};
static const char *const stereo_names[] = {"mono", "sbs", "tb"};
static const char *const levels_names[] = {"normal", "deep", "full"};
static const char *const source_names[] = {"default", "name", "image",
                                           "user"};

void title_settings_defaults(struct title_settings *settings)
{
    settings->projection_mode = NV12_PROJECTION_EQUIRECT_180;
    settings->stereo_layout = NV12_STEREO_SBS;
    settings->swap_eyes = false;
    settings->video_levels = NV12_LEVELS_NORMAL;
    settings->format_source = NV12_FORMAT_SOURCE_DEFAULT;
}

void title_settings_key(const char *input, char *key, size_t key_size)
{
    if (strncmp(input, "jf:item:", 8) == 0) {
        size_t length = strcspn(input + 8, ":");
        snprintf(key, key_size, "jf:%.*s", (int)length, input + 8);
        return;
    }
    char resolved[PATH_MAX];
    if (strstr(input, "://") == NULL && realpath(input, resolved))
        snprintf(key, key_size, "%s", resolved);
    else
        snprintf(key, key_size, "%s", input);
}

static int store_path(char *path, size_t size, bool create_directory)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char directory[PATH_MAX];
    if (base && base[0])
        snprintf(directory, sizeof(directory), "%s/stereonaut", base);
    else if (home && home[0])
        snprintf(directory, sizeof(directory), "%s/.config/stereonaut", home);
    else
        return -1;
    if (create_directory) {
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", directory);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            mkdir(parent, 0700);
        }
        if (mkdir(directory, 0700) < 0 && errno != EEXIST)
            return -1;
    }
    int written = snprintf(path, size, "%s/titles.json", directory);
    return written > 0 && (size_t)written < size ? 0 : -1;
}

/* Returns the parsed store, a fresh object if the file does not exist, or
 * NULL if it exists but is unusable (never overwrite what we cannot read). */
static json_object *read_store(const char *path)
{
    if (access(path, F_OK) < 0)
        return json_object_new_object();
    json_object *root = json_object_from_file(path);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root)
            json_object_put(root);
        return NULL;
    }
    return root;
}

/* Writes the store atomically; consumes `root`. */
static int write_store(const char *path, json_object *root)
{
    char temporary[PATH_MAX + 8];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    int result = -1;
    if (json_object_to_file_ext(temporary, root, JSON_C_TO_STRING_PRETTY) == 0) {
        chmod(temporary, 0600);
        result = rename(temporary, path);
    }
    if (result < 0)
        unlink(temporary);
    json_object_put(root);
    return result;
}

/* The entry of `key`, created if missing. */
static json_object *store_entry(json_object *root, const char *key)
{
    json_object *entry = NULL;
    if (!json_object_object_get_ex(root, key, &entry) ||
        !json_object_is_type(entry, json_type_object)) {
        entry = json_object_new_object();
        json_object_object_add(root, key, entry);
    }
    return entry;
}

static uint32_t lookup(const char *const *names, size_t count,
                       const char *text, uint32_t fallback)
{
    for (size_t i = 0; i < count; ++i)
        if (strcmp(names[i], text) == 0)
            return (uint32_t)i;
    return fallback;
}

int title_settings_load(const char *key, struct title_settings *settings)
{
    char path[PATH_MAX];
    if (store_path(path, sizeof(path), false) < 0)
        return -1;
    json_object *root = read_store(path);
    if (!root)
        return -1;
    json_object *entry = NULL, *value = NULL;
    int result = 0;
    if (json_object_object_get_ex(root, key, &entry) &&
        json_object_is_type(entry, json_type_object)) {
        if (json_object_object_get_ex(entry, "projection", &value) &&
            json_object_is_type(value, json_type_string)) {
            settings->projection_mode = lookup(
                projection_names, 3, json_object_get_string(value),
                settings->projection_mode);
            settings->format_source = NV12_FORMAT_SOURCE_USER;
        }
        if (json_object_object_get_ex(entry, "format_source", &value) &&
            json_object_is_type(value, json_type_string))
            settings->format_source = lookup(
                source_names, 4, json_object_get_string(value),
                settings->format_source);
        if (json_object_object_get_ex(entry, "stereo", &value) &&
            json_object_is_type(value, json_type_string))
            settings->stereo_layout = lookup(
                stereo_names, 3, json_object_get_string(value),
                settings->stereo_layout);
        if (json_object_object_get_ex(entry, "swap_eyes", &value) &&
            json_object_is_type(value, json_type_boolean))
            settings->swap_eyes = json_object_get_boolean(value);
        if (json_object_object_get_ex(entry, "levels", &value) &&
            json_object_is_type(value, json_type_string))
            settings->video_levels = lookup(
                levels_names, 3, json_object_get_string(value),
                settings->video_levels);
        result = 1;
    }
    json_object_put(root);
    return result;
}

int title_settings_save(const char *key, const struct title_settings *settings)
{
    if (settings->projection_mode > NV12_PROJECTION_EQUIRECT_360 ||
        settings->stereo_layout > NV12_STEREO_TB ||
        settings->video_levels > NV12_LEVELS_FULL ||
        settings->format_source > NV12_FORMAT_SOURCE_USER)
        return -1;
    char path[PATH_MAX];
    if (store_path(path, sizeof(path), true) < 0)
        return -1;
    json_object *root = read_store(path);
    if (!root)
        return -1;
    json_object *entry = store_entry(root, key);
    json_object_object_add(
        entry, "projection",
        json_object_new_string(projection_names[settings->projection_mode]));
    json_object_object_add(
        entry, "stereo",
        json_object_new_string(stereo_names[settings->stereo_layout]));
    json_object_object_add(entry, "swap_eyes",
                           json_object_new_boolean(settings->swap_eyes));
    json_object_object_add(
        entry, "levels",
        json_object_new_string(levels_names[settings->video_levels]));
    json_object_object_add(
        entry, "format_source",
        json_object_new_string(source_names[settings->format_source]));
    return write_store(path, root);
}

int title_settings_load_numbers(const char *key, const char *group,
                                const char *const *names, float *values,
                                size_t count)
{
    char path[PATH_MAX];
    if (store_path(path, sizeof(path), false) < 0)
        return -1;
    json_object *root = read_store(path);
    if (!root)
        return -1;
    json_object *entry = NULL, *numbers = NULL, *value = NULL;
    int result = 0;
    if (json_object_object_get_ex(root, key, &entry) &&
        json_object_is_type(entry, json_type_object) &&
        json_object_object_get_ex(entry, group, &numbers) &&
        json_object_is_type(numbers, json_type_object)) {
        for (size_t i = 0; i < count; ++i)
            if (names[i] &&
                json_object_object_get_ex(numbers, names[i], &value) &&
                (json_object_is_type(value, json_type_double) ||
                 json_object_is_type(value, json_type_int)))
                values[i] = (float)json_object_get_double(value);
        result = 1;
    }
    json_object_put(root);
    return result;
}

int title_settings_save_numbers(const char *key, const char *group,
                                const char *const *names,
                                const float *values, size_t count)
{
    char path[PATH_MAX];
    if (store_path(path, sizeof(path), true) < 0)
        return -1;
    json_object *root = read_store(path);
    if (!root)
        return -1;
    json_object *numbers = json_object_new_object();
    for (size_t i = 0; i < count; ++i)
        if (names[i])
            json_object_object_add(numbers, names[i],
                                   json_object_new_double(values[i]));
    json_object_object_add(store_entry(root, key), group, numbers);
    return write_store(path, root);
}
