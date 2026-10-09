#ifndef STEREONAUT_JELLYFIN_H
#define STEREONAUT_JELLYFIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Blocking Jellyfin REST client (libcurl + json-c). Every call may take
 * seconds; callers run it on a worker thread. Error strings are German and
 * shown in the browser. */

#define JF_ID_SIZE 48
#define JF_AUTH_RETRY (-2) /* token missing, expired or rejected */
#define JF_BROKEN_ITEM (-3) /* the server's file has no video stream */

struct jf_config {
    char server[256];
    char device_id[64];
    char user_id[64];
    char token[256];
};

struct jf_item {
    char id[JF_ID_SIZE];
    char name[256];
    bool is_folder;
    /* Image path below /Items/ID/Images/ for tiles: a landscape backdrop if
     * there is one, otherwise the primary image ("" if neither). */
    char image[96];
    int64_t runtime_ticks; /* 0 if unknown; 10 000 000 ticks per second */
    int year;              /* 0 if unknown */
};

struct jf_playback {
    char url[2304];
    char play_session[128];
    bool transcoding;
};

/* Reads $XDG_CONFIG_HOME/stereonaut/jellyfin.json. Returns 0 if a server is
 * configured (token may still be empty), -1 otherwise. */
int jf_config_load(struct jf_config *config, char *error, size_t error_size);
bool jf_config_has_token(const struct jf_config *config);

/* Quick Connect: start returns the code to show and a secret to poll with.
 * Poll sets *authenticated and, on success, stores the token on disk. */
int jf_quickconnect_start(struct jf_config *config, char code[16],
                          char secret[128], char *error, size_t error_size);
int jf_quickconnect_poll(struct jf_config *config, const char *secret,
                         bool *authenticated, char *error, size_t error_size);

/* Libraries (views) and the children of a folder, sorted by name. The array
 * is freed with free(). */
int jf_list_views(const struct jf_config *config, struct jf_item **items,
                  size_t *count, char *error, size_t error_size);
int jf_list_children(const struct jf_config *config, const char *parent_id,
                     struct jf_item **items, size_t *count,
                     char *error, size_t error_size);
/* Name and parent id of an item (parent may be empty). */
int jf_item_info(const struct jf_config *config, const char *id,
                 char *name, size_t name_size, char *parent,
                 size_t parent_size, char *error, size_t error_size);

/* Chooses Direct Play for HEVC Main 8 bit in mkv/mp4 and an HLS transcode to
 * HEVC Main 8 bit otherwise. The URL carries no credentials. */
int jf_resolve_playback(const struct jf_config *config, const char *item_id,
                        struct jf_playback *playback, char *error,
                        size_t error_size);
/* Downloads an item image (`image` as in jf_item) scaled to `max_width`.
 * On success *data is malloc'd and owned by the caller. */
int jf_fetch_image(const struct jf_config *config, const char *item_id,
                   const char *image, int max_width, char **data,
                   size_t *size);
/* Stops the server-side transcode of a finished or restarted playback. */
void jf_stop_encoding(const struct jf_config *config,
                      const struct jf_playback *playback);
/* "Authorization: MediaBrowser ..." line for STEREONAUT_HTTP_HEADERS. */
void jf_authorization_header(const struct jf_config *config, char *out,
                             size_t out_size);

#endif
