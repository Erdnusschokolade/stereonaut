#ifndef STEREONAUT_FILE_BROWSER_H
#define STEREONAUT_FILE_BROWSER_H

#include <stddef.h>
#include <stdint.h>

enum file_browser_entry_kind {
    FILE_BROWSER_PARENT,
    FILE_BROWSER_DIRECTORY,
    FILE_BROWSER_MEDIA,
};

struct file_browser_entry {
    enum file_browser_entry_kind kind;
    char *name;
    char *path;
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
int file_browser_poll(struct file_browser *browser,
                      struct file_browser_listing *listing);
void file_browser_listing_clear(struct file_browser_listing *listing);
void file_browser_close(struct file_browser **browser);

#endif
