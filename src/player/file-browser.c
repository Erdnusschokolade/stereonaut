#define _XOPEN_SOURCE 700
#include "file-browser.h"
#include "jellyfin.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

struct file_browser {
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool worker_started;
    bool stopping;
    char *request_path;
    uint64_t request_generation;
    struct file_browser_listing ready;
    bool has_ready;
    int sort;
    bool descending;
    int local_sort;
    bool local_descending;
};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

void file_browser_listing_clear(struct file_browser_listing *listing)
{
    if (!listing)
        return;
    for (size_t i = 0; i < listing->entry_count; ++i) {
        free(listing->entries[i].name);
        free(listing->entries[i].path);
        free(listing->entries[i].image);
    }
    free(listing->entries);
    free(listing->path);
    memset(listing, 0, sizeof(*listing));
}

static bool is_media_name(const char *name)
{
    const char *extension = strrchr(name, '.');
    return extension &&
           (strcasecmp(extension, ".mkv") == 0 ||
            strcasecmp(extension, ".mp4") == 0);
}

static char *join_path(const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    bool root = directory_length == 1 && directory[0] == '/';
    size_t size = directory_length + (root ? 0U : 1U) + strlen(name) + 1U;
    char *path = malloc(size);
    if (!path)
        return NULL;
    snprintf(path, size, root ? "/%s" : "%s/%s", directory, name);
    return path;
}

/* Order for compare_entries; only the browser worker sorts. */
static int compare_sort = FILE_BROWSER_SORT_NAME;
static bool compare_descending;

static int compare_entries(const void *left_pointer, const void *right_pointer)
{
    const struct file_browser_entry *left = left_pointer;
    const struct file_browser_entry *right = right_pointer;
    if (left->kind != right->kind)
        return left->kind < right->kind ? -1 : 1;
    int order = 0;
    if (compare_sort == FILE_BROWSER_SORT_MODIFIED)
        order = (left->modified > right->modified) -
                (left->modified < right->modified);
    else if (compare_sort == FILE_BROWSER_SORT_SIZE &&
             left->kind == FILE_BROWSER_MEDIA)
        order = (left->size > right->size) - (left->size < right->size);
    if (order)
        return compare_descending ? -order : order;
    int folded = strcasecmp(left->name, right->name);
    return folded ? folded : strcmp(left->name, right->name);
}

static int append_entry(struct file_browser_listing *listing,
                        enum file_browser_entry_kind kind,
                        const char *name, char *path)
{
    size_t next_count = listing->entry_count + 1U;
    struct file_browser_entry *entries =
        realloc(listing->entries, next_count * sizeof(*entries));
    if (!entries) {
        free(path);
        return -1;
    }
    listing->entries = entries;
    struct file_browser_entry *entry = &entries[listing->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    entry->name = strdup(name);
    entry->path = path;
    if (!entry->name || !entry->path) {
        free(entry->name);
        free(entry->path);
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    listing->entry_count = next_count;
    return 0;
}

static void parent_path(const char *path, char *parent, size_t size)
{
    snprintf(parent, size, "%s", path);
    size_t length = strlen(parent);
    while (length > 1 && parent[length - 1U] == '/')
        parent[--length] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        snprintf(parent, size, "/");
    else
        *slash = '\0';
}

static struct file_browser_listing read_directory(const char *requested,
                                                   uint64_t generation,
                                                   int sort, bool descending)
{
    struct file_browser_listing listing = {.generation = generation};
    char *resolved = realpath(requested, NULL);
    if (!resolved) {
        snprintf(listing.error, sizeof(listing.error),
                 "Verzeichnis kann nicht geöffnet werden: %s", strerror(errno));
        listing.path = strdup(requested);
        return listing;
    }
    /* Display title "Lokale Dateien / home / user / Videos". */
    size_t title_size = strlen(resolved) * 2U + 32U;
    listing.path = malloc(title_size);
    if (listing.path) {
        size_t used = (size_t)snprintf(listing.path, title_size,
                                       "Lokale Dateien");
        for (const char *c = resolved; *c && used + 4U < title_size; ++c) {
            if (*c == '/') {
                if (c[1]) {
                    memcpy(listing.path + used, " / ", 3);
                    used += 3;
                }
            } else {
                listing.path[used++] = *c;
            }
        }
        listing.path[used] = '\0';
    }
    DIR *directory = opendir(resolved);
    if (!directory) {
        snprintf(listing.error, sizeof(listing.error),
                 "Verzeichnis kann nicht geöffnet werden: %s", strerror(errno));
        free(resolved);
        return listing;
    }
    if (strcmp(resolved, "/") != 0) {
        char parent[4096];
        parent_path(resolved, parent, sizeof(parent));
        if (append_entry(&listing, FILE_BROWSER_PARENT, "..",
                         strdup(parent)) < 0)
            goto allocation_failure;
    }
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(directory);
        if (!item) {
            if (errno)
                snprintf(listing.error, sizeof(listing.error),
                         "Verzeichnis konnte nicht vollständig gelesen werden: %s",
                         strerror(errno));
            break;
        }
        if (strcmp(item->d_name, ".") == 0 ||
            strcmp(item->d_name, "..") == 0)
            continue;
        char *path = join_path(resolved, item->d_name);
        if (!path)
            goto allocation_failure;
        struct stat status;
        if (stat(path, &status) < 0) {
            free(path);
            continue;
        }
        enum file_browser_entry_kind kind;
        if (S_ISDIR(status.st_mode))
            kind = FILE_BROWSER_DIRECTORY;
        else if (S_ISREG(status.st_mode) && is_media_name(item->d_name))
            kind = FILE_BROWSER_MEDIA;
        else {
            free(path);
            continue;
        }
        if (append_entry(&listing, kind, item->d_name, path) < 0)
            goto allocation_failure;
        listing.entries[listing.entry_count - 1U].modified =
            (int64_t)status.st_mtime;
        listing.entries[listing.entry_count - 1U].size =
            (int64_t)status.st_size;
    }
    closedir(directory);
    free(resolved);
    compare_sort = sort;
    compare_descending = descending;
    qsort(listing.entries, listing.entry_count, sizeof(*listing.entries),
          compare_entries);
    return listing;

allocation_failure:
    closedir(directory);
    free(resolved);
    file_browser_listing_clear(&listing);
    listing.generation = generation;
    listing.path = strdup(requested);
    snprintf(listing.error, sizeof(listing.error),
             "Nicht genug Speicher für Verzeichnisliste");
    return listing;
}

/* ---- virtual root and Jellyfin sources ---------------------------------- */

static char login_code[16];
static char login_secret[128];

static int add_entry(struct file_browser_listing *listing,
                     enum file_browser_entry_kind kind, const char *name,
                     const char *path)
{
    char *copy = strdup(path);
    return copy ? append_entry(listing, kind, name, copy) : -1;
}

/* Appends Jellyfin items; titles remember `folder_id` to return to after
 * playback ("" = the start page). */
static int add_jellyfin_items(struct file_browser_listing *listing,
                              const struct jf_item *items, size_t count,
                              const char *folder_id)
{
    for (size_t i = 0; i < count; ++i) {
        char path[4 * JF_ID_SIZE + 32];
        if (items[i].is_folder)
            snprintf(path, sizeof(path), "jf:view:%s", items[i].id);
        else
            snprintf(path, sizeof(path), "jf:item:%s:%s", items[i].id,
                     folder_id);
        if (add_entry(listing,
                      items[i].is_folder ? FILE_BROWSER_DIRECTORY
                                         : FILE_BROWSER_MEDIA,
                      items[i].name, path) < 0)
            return -1;
        struct file_browser_entry *entry =
            &listing->entries[listing->entry_count - 1U];
        if (items[i].image[0]) {
            size_t size = strlen(items[i].id) + strlen(items[i].image) + 2U;
            entry->image = malloc(size);
            if (entry->image)
                snprintf(entry->image, size, "%s|%s", items[i].id,
                         items[i].image);
        }
        entry->duration_us = items[i].runtime_ticks / 10;
        entry->year = items[i].year;
        snprintf(entry->codec, sizeof(entry->codec), "%s", items[i].codec);
        entry->width = items[i].width;
        entry->height = items[i].height;
        entry->resume_us = items[i].resume_ticks / 10;
        entry->played_percentage = items[i].played_percentage;
        entry->played = items[i].played;
    }
    return 0;
}

/* Start page: the two sources, then Jellyfin's "continue watching" titles
 * (skipped quietly if the server is not configured or not reachable). */
static struct file_browser_listing read_root(uint64_t generation)
{
    struct file_browser_listing listing = {.generation = generation};
    listing.path = strdup("Start");
    const char *home = getenv("HOME");
    if (add_entry(&listing, FILE_BROWSER_DIRECTORY, "Lokale Dateien",
                  home && home[0] ? home : "/") < 0 ||
        add_entry(&listing, FILE_BROWSER_DIRECTORY, "Jellyfin", "jf:") < 0) {
        file_browser_listing_clear(&listing);
        listing.generation = generation;
        snprintf(listing.error, sizeof(listing.error), "Nicht genug Speicher");
        return listing;
    }
    struct jf_config config;
    struct jf_item *items = NULL;
    size_t count = 0;
    if (jf_config_load(&config, NULL, 0) == 0 &&
        jf_config_has_token(&config) &&
        jf_list_resume(&config, &items, &count, NULL, 0) == 0)
        add_jellyfin_items(&listing, items, count, "");
    free(items);
    return listing;
}

static struct file_browser_listing jellyfin_message(
    uint64_t generation, const char *title, const char *message)
{
    struct file_browser_listing listing = {.generation = generation};
    listing.path = strdup(title);
    snprintf(listing.error, sizeof(listing.error), "%s", message);
    add_entry(&listing, FILE_BROWSER_PARENT, "..", "root:");
    return listing;
}

static struct file_browser_listing jellyfin_login(
    uint64_t generation, struct jf_config *config, bool poll,
    const char *note)
{
    char error[256] = {0};
    if (poll && login_secret[0]) {
        bool authenticated = false;
        if (jf_quickconnect_poll(config, login_secret, &authenticated, error,
                                 sizeof(error)) == 0 && authenticated) {
            login_secret[0] = '\0';
            return (struct file_browser_listing){.generation = generation};
        }
        if (!error[0])
            note = "Noch nicht bestätigt";
        else
            login_secret[0] = '\0'; /* expired or rejected: request a new code */
    }
    if (!login_secret[0] &&
        jf_quickconnect_start(config, login_code, login_secret, error,
                              sizeof(error)) < 0)
        return jellyfin_message(generation, "Jellyfin anmelden",
                                error[0] ? error : "Anmeldung nicht möglich");
    struct file_browser_listing listing = {.generation = generation};
    listing.path = strdup("Jellyfin anmelden");
    snprintf(listing.error, sizeof(listing.error), "%s",
             note ? note : "Code in Jellyfin unter Quick Connect bestätigen");
    char label[64];
    add_entry(&listing, FILE_BROWSER_PARENT, "..", "root:");
    snprintf(label, sizeof(label), "Code %s", login_code);
    add_entry(&listing, FILE_BROWSER_DIRECTORY, label, "jf:login");
    add_entry(&listing, FILE_BROWSER_DIRECTORY, "Weiter nach Bestätigung",
              "jf:login");
    return listing;
}

static const char *const filter_keys[JF_FILTER_COUNT] = {
    "", "genre", "studio", "person",
};
static const char *const filter_names[JF_FILTER_COUNT] = {
    "", "Genre", "Studio", "Darsteller",
};
static const char *const filter_titles[JF_FILTER_COUNT] = {
    "", "Genres", "Studios", "Darsteller",
};

/* "genre=ID" (id copied when `id` is given) or "genre" alone. */
static enum jf_filter parse_filter(const char *text, char *id,
                                   size_t id_size)
{
    for (int filter = JF_FILTER_GENRE; filter < JF_FILTER_COUNT; ++filter) {
        size_t length = strlen(filter_keys[filter]);
        if (strncmp(text, filter_keys[filter], length) != 0)
            continue;
        if (!id && text[length] == '\0')
            return (enum jf_filter)filter;
        if (id && text[length] == '=' && text[length + 1] &&
            strlen(text + length + 1) < id_size) {
            snprintf(id, id_size, "%s", text + length + 1);
            return (enum jf_filter)filter;
        }
    }
    return JF_FILTER_NONE;
}

/* Genres, studios or persons as folders that open the filtered listing. */
static int add_filter_values(struct file_browser_listing *listing,
                             const struct jf_item *items, size_t count,
                             const char *folder_id, enum jf_filter filter)
{
    for (size_t i = 0; i < count; ++i) {
        char path[3 * JF_ID_SIZE + 32];
        snprintf(path, sizeof(path), "jf:view:%s/%s=%s", folder_id,
                 filter_keys[filter], items[i].id);
        if (add_entry(listing, FILE_BROWSER_DIRECTORY, items[i].name,
                      path) < 0)
            return -1;
        struct file_browser_entry *entry =
            &listing->entries[listing->entry_count - 1U];
        if (items[i].image[0]) {
            size_t size = strlen(items[i].id) + strlen(items[i].image) + 2U;
            entry->image = malloc(size);
            if (entry->image)
                snprintf(entry->image, size, "%s|%s", items[i].id,
                         items[i].image);
        }
    }
    return 0;
}

static struct file_browser_listing read_jellyfin(const char *request,
                                                  uint64_t generation,
                                                  enum jf_sort sort,
                                                  bool descending)
{
    char error[256] = {0};
    struct jf_config config;
    if (jf_config_load(&config, error, sizeof(error)) < 0)
        return jellyfin_message(generation, "Jellyfin", error);
    bool login_request = strcmp(request, "jf:login") == 0;
    if (login_request || !jf_config_has_token(&config)) {
        struct file_browser_listing login =
            jellyfin_login(generation, &config, login_request, NULL);
        if (login.path)
            return login;
        request = "jf:"; /* authenticated just now */
    }

    struct file_browser_listing listing = {.generation = generation};
    struct jf_item *items = NULL;
    size_t count = 0;
    char title[320] = "Jellyfin";
    char parent_path_text[JF_ID_SIZE + 16] = "root:";
    const char *folder_id = NULL;
    /* "jf:view:FOLDER/genre=ID" lists a folder's titles of one genre (or
     * studio, person); "jf:filters:FOLDER:genre" the genres to pick from. */
    char folder[JF_ID_SIZE] = "", filter_id[JF_ID_SIZE] = "";
    enum jf_filter filter = JF_FILTER_NONE;
    bool filter_values = false;
    if (strncmp(request, "jf:view:", 8) == 0 ||
        strncmp(request, "jf:filters:", 11) == 0) {
        filter_values = request[3] == 'f';
        const char *start = request + (filter_values ? 11 : 8);
        size_t length = strcspn(start, filter_values ? ":" : "/");
        if (length < sizeof(folder)) {
            memcpy(folder, start, length);
            folder[length] = '\0';
        }
        folder_id = folder;
        const char *rest = start + length;
        if (*rest)
            ++rest;
        filter = parse_filter(rest, filter_values ? NULL : filter_id,
                              sizeof(filter_id));
        if (filter_values && filter == JF_FILTER_NONE)
            filter_values = false;
    }
    int result;
    if (!folder_id) {
        result = jf_list_views(&config, &items, &count, error, sizeof(error));
    } else {
        struct jf_item *views = NULL;
        size_t view_count = 0;
        bool is_view = false;
        if (jf_list_views(&config, &views, &view_count, error,
                          sizeof(error)) == 0) {
            for (size_t i = 0; i < view_count; ++i)
                if (strcmp(views[i].id, folder_id) == 0) {
                    is_view = true;
                    snprintf(title, sizeof(title), "Jellyfin / %s",
                             views[i].name);
                }
            free(views);
        }
        if (is_view) {
            snprintf(parent_path_text, sizeof(parent_path_text), "jf:");
        } else {
            char name[256] = {0}, parent[JF_ID_SIZE] = {0};
            if (jf_item_info(&config, folder_id, name, sizeof(name), parent,
                             sizeof(parent), error, sizeof(error)) == 0) {
                snprintf(title, sizeof(title), "Jellyfin / %s", name);
                if (parent[0])
                    snprintf(parent_path_text, sizeof(parent_path_text),
                             "jf:view:%s", parent);
                else
                    snprintf(parent_path_text, sizeof(parent_path_text), "jf:");
            }
        }
        error[0] = '\0';
        if (filter != JF_FILTER_NONE) {
            /* Filters belong to the folder: ".." leads back to it. */
            snprintf(parent_path_text, sizeof(parent_path_text),
                     "jf:view:%s", folder_id);
            size_t used = strlen(title);
            if (filter_values) {
                snprintf(title + used, sizeof(title) - used, " / %s",
                         filter_titles[filter]);
            } else {
                char name[256] = {0};
                jf_item_info(&config, filter_id, name, sizeof(name), NULL, 0,
                             NULL, 0);
                snprintf(title + used, sizeof(title) - used, " / %s: %s",
                         filter_names[filter], name[0] ? name : "?");
            }
        }
        if (filter_values)
            result = jf_list_filter_values(&config, folder_id, filter,
                                           &items, &count, error,
                                           sizeof(error));
        else
            result = jf_list_children(&config, folder_id, filter, filter_id,
                                      sort, descending, &items, &count,
                                      error, sizeof(error));
    }
    if (result == JF_AUTH_RETRY) {
        config.token[0] = '\0';
        login_secret[0] = '\0';
        return jellyfin_login(generation, &config, false,
                              "Anmeldung abgelaufen: neuen Code bestätigen");
    }
    if (result < 0)
        return jellyfin_message(generation, "Jellyfin",
                                error[0] ? error : "Jellyfin-Fehler");
    listing.path = strdup(title);
    if (add_entry(&listing, FILE_BROWSER_PARENT, "..", parent_path_text) < 0)
        goto allocation_failure;
    if (filter_values) {
        if (add_filter_values(&listing, items, count, folder_id, filter) < 0)
            goto allocation_failure;
    } else {
        /* Titles remember the filtered folder to come back to. */
        char back[2 * JF_ID_SIZE + 16];
        snprintf(back, sizeof(back), "%s", folder_id ? folder_id : "");
        if (filter != JF_FILTER_NONE) {
            size_t used = strlen(back);
            snprintf(back + used, sizeof(back) - used, "/%s=%s",
                     filter_keys[filter], filter_id);
        }
        if (add_jellyfin_items(&listing, items, count, back) < 0)
            goto allocation_failure;
    }
    free(items);
    return listing;

allocation_failure:
    free(items);
    file_browser_listing_clear(&listing);
    listing.generation = generation;
    listing.path = strdup("Jellyfin");
    snprintf(listing.error, sizeof(listing.error), "Nicht genug Speicher");
    return listing;
}

static struct file_browser_listing read_location(
    const char *path, uint64_t generation, int sort, bool descending,
    int local_sort, bool local_descending)
{
    if (strcmp(path, "root:") == 0)
        return read_root(generation);
    if (strncmp(path, "jf:", 3) == 0)
        return read_jellyfin(path, generation, (enum jf_sort)sort,
                             descending);
    return read_directory(path, generation, local_sort, local_descending);
}

static void *browser_worker(void *argument)
{
    struct file_browser *browser = argument;
    for (;;) {
        pthread_mutex_lock(&browser->mutex);
        while (!browser->request_path && !browser->stopping)
            pthread_cond_wait(&browser->changed, &browser->mutex);
        if (browser->stopping) {
            pthread_mutex_unlock(&browser->mutex);
            return NULL;
        }
        char *path = browser->request_path;
        uint64_t generation = browser->request_generation;
        int sort = browser->sort;
        bool descending = browser->descending;
        int local_sort = browser->local_sort;
        bool local_descending = browser->local_descending;
        browser->request_path = NULL;
        pthread_mutex_unlock(&browser->mutex);

        struct file_browser_listing listing =
            read_location(path, generation, sort, descending, local_sort,
                          local_descending);
        free(path);

        pthread_mutex_lock(&browser->mutex);
        if (generation == browser->request_generation) {
            if (browser->has_ready)
                file_browser_listing_clear(&browser->ready);
            browser->ready = listing;
            browser->has_ready = true;
            memset(&listing, 0, sizeof(listing));
        }
        pthread_mutex_unlock(&browser->mutex);
        file_browser_listing_clear(&listing);
    }
}

void file_browser_set_sort(struct file_browser *browser, int sort,
                           bool descending)
{
    if (!browser)
        return;
    pthread_mutex_lock(&browser->mutex);
    browser->sort = sort;
    browser->descending = descending;
    pthread_mutex_unlock(&browser->mutex);
}

void file_browser_set_local_sort(struct file_browser *browser, int sort,
                                 bool descending)
{
    if (!browser)
        return;
    pthread_mutex_lock(&browser->mutex);
    browser->local_sort = sort >= 0 && sort < FILE_BROWSER_SORT_COUNT
                              ? sort
                              : FILE_BROWSER_SORT_NAME;
    browser->local_descending = descending;
    pthread_mutex_unlock(&browser->mutex);
}

int file_browser_request(struct file_browser *browser, const char *path,
                         char *error, size_t error_size)
{
    if (!browser || !path || !path[0]) {
        set_error(error, error_size, "ungültiger Verzeichnispfad");
        return -1;
    }
    char *copy = strdup(path);
    if (!copy) {
        set_error(error, error_size, "Verzeichnispfad konnte nicht kopiert werden");
        return -1;
    }
    pthread_mutex_lock(&browser->mutex);
    free(browser->request_path);
    browser->request_path = copy;
    ++browser->request_generation;
    pthread_cond_signal(&browser->changed);
    pthread_mutex_unlock(&browser->mutex);
    return 0;
}

int file_browser_open(struct file_browser **result, const char *initial_path,
                      char *error, size_t error_size)
{
    if (!result) {
        set_error(error, error_size, "ungültiger Browser-Zeiger");
        return -1;
    }
    *result = NULL;
    struct file_browser *browser = calloc(1, sizeof(*browser));
    if (!browser) {
        set_error(error, error_size, "Browser konnte nicht angelegt werden");
        return -1;
    }
    if (pthread_mutex_init(&browser->mutex, NULL) != 0) {
        set_error(error, error_size, "Browser-Synchronisierung fehlgeschlagen");
        free(browser);
        return -1;
    }
    if (pthread_cond_init(&browser->changed, NULL) != 0) {
        set_error(error, error_size, "Browser-Synchronisierung fehlgeschlagen");
        pthread_mutex_destroy(&browser->mutex);
        free(browser);
        return -1;
    }
    if (pthread_create(&browser->worker, NULL, browser_worker, browser) != 0) {
        set_error(error, error_size, "Browser-Thread konnte nicht starten");
        pthread_cond_destroy(&browser->changed);
        pthread_mutex_destroy(&browser->mutex);
        free(browser);
        return -1;
    }
    browser->worker_started = true;
    *result = browser;
    if (file_browser_request(browser, initial_path, error, error_size) < 0) {
        file_browser_close(result);
        return -1;
    }
    return 0;
}

int file_browser_poll(struct file_browser *browser,
                      struct file_browser_listing *listing)
{
    if (!browser || !listing)
        return -1;
    pthread_mutex_lock(&browser->mutex);
    if (!browser->has_ready) {
        pthread_mutex_unlock(&browser->mutex);
        return 0;
    }
    *listing = browser->ready;
    memset(&browser->ready, 0, sizeof(browser->ready));
    browser->has_ready = false;
    pthread_mutex_unlock(&browser->mutex);
    return 1;
}

void file_browser_close(struct file_browser **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct file_browser *browser = *pointer;
    pthread_mutex_lock(&browser->mutex);
    browser->stopping = true;
    pthread_cond_broadcast(&browser->changed);
    pthread_mutex_unlock(&browser->mutex);
    if (browser->worker_started)
        pthread_join(browser->worker, NULL);
    free(browser->request_path);
    if (browser->has_ready)
        file_browser_listing_clear(&browser->ready);
    pthread_cond_destroy(&browser->changed);
    pthread_mutex_destroy(&browser->mutex);
    free(browser);
    *pointer = NULL;
}
