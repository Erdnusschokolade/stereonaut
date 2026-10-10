#ifndef STEREONAUT_FILE_BROWSER_H
#define STEREONAUT_FILE_BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Order of local directories (folders always first). */
enum file_browser_local_sort {
    FILE_BROWSER_SORT_NAME,
    FILE_BROWSER_SORT_MODIFIED,
    FILE_BROWSER_SORT_SIZE,
    FILE_BROWSER_SORT_COUNT,
};

enum file_browser_entry_kind {
    FILE_BROWSER_PARENT,
    FILE_BROWSER_DIRECTORY,
    FILE_BROWSER_MEDIA,
};

struct file_browser_entry {
    enum file_browser_entry_kind kind;
    char *name; /* UTF-8 */
    char *path;
    /* Thumbnail source "ITEM_ID|IMAGE" for Jellyfin entries (see
     * jf_fetch_image), NULL otherwise. */
    char *image;
    int64_t duration_us; /* 0 if unknown */
    int year;            /* 0 if unknown */
    char codec[32];      /* e.g. "HEVC Main 10", "" if unknown */
    int width, height;   /* 0 if unknown */
    /* Jellyfin user state: resume position (0 = none), share watched in
     * percent, played to the end. */
    int64_t resume_us;
    float played_percentage;
    bool played;
    /* Local files: modification time (seconds since 1970) and size. */
    int64_t modified;
    int64_t size;
};

struct file_browser_listing {
    uint64_t generation;
    char *path;
    struct file_browser_entry *entries;
    size_t entry_count;
    char error[256];
};

struct file_browser;

int file_browser_open(struct file_browser **result, const char *initial_path,
                      char *error, size_t error_size);
int file_browser_request(struct file_browser *browser, const char *path,
                         char *error, size_t error_size);
/* Order of Jellyfin folder listings (enum jf_sort) and of local
 * directories (enum file_browser_local_sort); applies to the next
 * request. */
void file_browser_set_sort(struct file_browser *browser, int sort,
                           bool descending);
void file_browser_set_local_sort(struct file_browser *browser, int sort,
                                 bool descending);
int file_browser_poll(struct file_browser *browser,
                      struct file_browser_listing *listing);
void file_browser_listing_clear(struct file_browser_listing *listing);
void file_browser_close(struct file_browser **browser);

#endif
