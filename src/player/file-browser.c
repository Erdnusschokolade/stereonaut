#define _XOPEN_SOURCE 700
#include "file-browser.h"

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

static int compare_entries(const void *left_pointer, const void *right_pointer)
{
    const struct file_browser_entry *left = left_pointer;
    const struct file_browser_entry *right = right_pointer;
    if (left->kind != right->kind)
        return left->kind < right->kind ? -1 : 1;
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
                                                   uint64_t generation)
{
    struct file_browser_listing listing = {.generation = generation};
    char *resolved = realpath(requested, NULL);
    if (!resolved) {
        snprintf(listing.error, sizeof(listing.error),
                 "Verzeichnis kann nicht geöffnet werden: %s", strerror(errno));
        listing.path = strdup(requested);
        return listing;
    }
    listing.path = resolved;
    DIR *directory = opendir(resolved);
    if (!directory) {
        snprintf(listing.error, sizeof(listing.error),
                 "Verzeichnis kann nicht geöffnet werden: %s", strerror(errno));
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
    }
    closedir(directory);
    qsort(listing.entries, listing.entry_count, sizeof(*listing.entries),
          compare_entries);
    return listing;

allocation_failure:
    closedir(directory);
    file_browser_listing_clear(&listing);
    listing.generation = generation;
    listing.path = strdup(requested);
    snprintf(listing.error, sizeof(listing.error),
             "Nicht genug Speicher für Verzeichnisliste");
    return listing;
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
        browser->request_path = NULL;
        pthread_mutex_unlock(&browser->mutex);

        struct file_browser_listing listing =
            read_directory(path, generation);
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
