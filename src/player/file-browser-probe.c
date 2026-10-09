#define _POSIX_C_SOURCE 200809L
#include "file-browser.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s DIRECTORY\n", argv[0]);
        return 2;
    }
    char error[256] = {0};
    struct file_browser *browser = NULL;
    if (file_browser_open(&browser, argv[1], error, sizeof(error)) < 0) {
        fprintf(stderr, "browser_error=%s\n", error);
        return 1;
    }
    struct file_browser_listing listing = {0};
    for (unsigned attempt = 0; attempt < 200U; ++attempt) {
        int result = file_browser_poll(browser, &listing);
        if (result < 0) {
            file_browser_close(&browser);
            return 1;
        }
        if (result > 0)
            break;
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    if (!listing.path) {
        fprintf(stderr, "browser_timeout=1\n");
        file_browser_close(&browser);
        return 1;
    }
    printf("listing generation=%" PRIu64 " path=%s entries=%zu error=%s\n",
           listing.generation, listing.path, listing.entry_count,
           listing.error[0] ? listing.error : "none");
    for (size_t i = 0; i < listing.entry_count; ++i)
        printf("entry kind=%u name=%s path=%s\n",
               (unsigned)listing.entries[i].kind,
               listing.entries[i].name, listing.entries[i].path);
    bool success = !listing.error[0];
    file_browser_listing_clear(&listing);
    file_browser_close(&browser);
    return success ? 0 : 1;
}
