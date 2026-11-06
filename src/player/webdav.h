#ifndef STEREONAUT_WEBDAV_H
#define STEREONAUT_WEBDAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Saved WebDAV servers ($XDG_CONFIG_HOME/stereonaut/webdav.json, 0600) and
 * PROPFIND directory listings over libcurl (blocking: callers run it on a
 * worker thread). Files play through the session's http(s) path;
 * wd_login_url() hands the credentials to libavformat inside the process,
 * never through argv. */

#define WD_MAX_SERVERS 16

struct wd_server {
    /* Names the server in "webdav:ID:..." locations (and so in titles.json
     * keys); stays when other servers are removed and is never reused. */
    int id;
    char name[64];
    char url[512]; /* base URL, e.g. https://host:8443/dav */
    char user[128];
    char pass[256];
};

/* Fills `out` with the saved servers (up to `max`); returns the count. */
int wd_servers_load(struct wd_server *out, int max);
/* Replaces the server with id `id`, or appends it with a new id for -1,
 * and saves the list (server->id is ignored); http:// is assumed without a
 * scheme, an empty name keeps the old one or becomes the address. 0 on
 * success, <0 with a message in `error`. */
int wd_server_save(int id, const struct wd_server *server, char *error,
                   size_t error_size);
/* Removes the server with id `id`; the others keep their ids. */
int wd_server_remove(int id, char *error, size_t error_size);
/* Copies the server with id `id` into `out`. 0 on success, <0 if there is
 * none. */
int wd_server_get(int id, struct wd_server *out);

/* scheme://user:pass@host[:port]`path` into `out` (user and password
 * percent-encoded), without a login just the URL: libavformat then answers
 * Basic and Digest challenges like libcurl does for the listings. Holds
 * the password, so never log it or pass it on the command line. */
void wd_login_url(const struct wd_server *server, const char *path,
                  char *out, size_t size);
/* scheme://host[:port] of the server's URL into `out`. */
void wd_origin(const struct wd_server *server, char *out, size_t size);
/* Path of the server's URL below the origin with a trailing slash ("/" if
 * none) into `out`. */
void wd_base_path(const struct wd_server *server, char *out, size_t size);
/* `in` with %XX escapes decoded into `out`. */
void wd_url_decode(const char *in, char *out, size_t size);

struct wd_entry {
    char name[256];  /* display name (decoded last path segment) */
    char path[1024]; /* encoded path below the origin, e.g. /dav/movies/ */
    bool is_dir;
    int64_t size;    /* bytes, 0 if unknown */
    /* A title's sidecar picture (see local-thumbnail.c), encoded like
     * `path`, and its size; "" if there is none. */
    char image[1024];
    int64_t image_size;
};

/* GET of a small file (a picture) below the origin into a malloc'd
 * buffer. 0 on success, <0 on any failure. */
int wd_fetch(const struct wd_server *server, const char *path, char **data,
             size_t *size);

#define WD_LOGIN_REJECTED (-2) /* HTTP 401 */
#define WD_UNREACHABLE (-3)    /* no connection, or HTTP 404 */

/* PROPFIND Depth 1 of `dir_path` (encoded, below the origin): folders and
 * .mkv/.mp4 files, without the folder itself. *out is malloc'd (caller
 * frees). 0 on success, <0 (WD_LOGIN_REJECTED, WD_UNREACHABLE or -1) with
 * a message in `error`. */
int wd_list(const struct wd_server *server, const char *dir_path,
            struct wd_entry **out, size_t *count, char *error,
            size_t error_size);

#endif
