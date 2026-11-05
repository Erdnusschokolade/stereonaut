#define _POSIX_C_SOURCE 200809L
#include "webdav.h"

#include "i18n.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* The PROPFIND reply is scanned, not parsed as a document: every
 * <response> has one <href>, and its properties run to the next href.
 * Namespace prefixes differ between servers (D:, d:, lp1:), so tags are
 * matched by their local name. */

static void set_error(char *error, size_t size, const char *message)
{
    if (error && size)
        snprintf(error, size, "%s", message);
}

/* ---- configuration ------------------------------------------------------ */

static int config_path(char *path, size_t size, bool directory_only)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    char fallback[512];
    if (!base || !base[0]) {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return -1;
        snprintf(fallback, sizeof(fallback), "%s/.config", home);
        base = fallback;
    }
    int written = snprintf(path, size, "%s/stereonaut%s", base,
                           directory_only ? "" : "/webdav.json");
    return written > 0 && (size_t)written < size ? 0 : -1;
}

static const char *json_string(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !value ||
        !json_object_is_type(value, json_type_string))
        return NULL;
    return json_object_get_string(value);
}

static void copy_string(char *target, size_t size, const char *source)
{
    snprintf(target, size, "%.*s", size ? (int)(size - 1) : 0,
             source ? source : "");
}

int wd_servers_load(struct wd_server *out, int max)
{
    char path[640];
    if (config_path(path, sizeof(path), false) < 0)
        return 0;
    json_object *root = json_object_from_file(path);
    if (!root)
        return 0;
    int count = 0;
    if (json_object_is_type(root, json_type_array)) {
        int n = (int)json_object_array_length(root);
        for (int i = 0; i < n && count < max; ++i) {
            json_object *item = json_object_array_get_idx(root, i);
            if (!item || !json_object_is_type(item, json_type_object))
                continue;
            struct wd_server *server = &out[count];
            memset(server, 0, sizeof(*server));
            copy_string(server->name, sizeof(server->name),
                        json_string(item, "name"));
            copy_string(server->url, sizeof(server->url),
                        json_string(item, "url"));
            copy_string(server->user, sizeof(server->user),
                        json_string(item, "user"));
            copy_string(server->pass, sizeof(server->pass),
                        json_string(item, "password"));
            if (!server->url[0])
                continue;
            if (!server->name[0])
                copy_string(server->name, sizeof(server->name), server->url);
            ++count;
        }
    }
    json_object_put(root);
    return count;
}

static int servers_save(const struct wd_server *servers, int count,
                        char *error, size_t error_size)
{
    char directory[640], path[640];
    if (config_path(directory, sizeof(directory), true) < 0 ||
        config_path(path, sizeof(path), false) < 0) {
        set_error(error, error_size, tr("No configuration directory"));
        return -1;
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        set_error(error, error_size, tr("Cannot write the configuration"));
        return -1;
    }
    json_object *array = json_object_new_array();
    if (!array) {
        set_error(error, error_size, tr("Not enough memory"));
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        json_object *item = json_object_new_object();
        if (!item) {
            json_object_put(array);
            set_error(error, error_size, tr("Not enough memory"));
            return -1;
        }
        json_object_object_add(item, "name",
                               json_object_new_string(servers[i].name));
        json_object_object_add(item, "url",
                               json_object_new_string(servers[i].url));
        json_object_object_add(item, "user",
                               json_object_new_string(servers[i].user));
        json_object_object_add(item, "password",
                               json_object_new_string(servers[i].pass));
        json_object_array_add(array, item);
    }
    const char *text =
        json_object_to_json_string_ext(array, JSON_C_TO_STRING_PRETTY);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = fd >= 0 && text &&
              write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    if (fd >= 0 && close(fd) < 0)
        ok = false;
    json_object_put(array);
    if (!ok) {
        set_error(error, error_size, tr("Cannot write the configuration"));
        return -1;
    }
    return 0;
}

/* The name a server gets without one of its own: the address without the
 * scheme and a trailing slash. */
static void default_name(const char *url, char *name, size_t size)
{
    const char *scheme = strstr(url, "://");
    const char *start = scheme ? scheme + 3 : url;
    size_t length = strlen(start);
    while (length > 1 && start[length - 1] == '/')
        --length;
    snprintf(name, size, "%.*s", (int)length, start);
}

int wd_server_save(int index, const struct wd_server *server, char *error,
                   size_t error_size)
{
    if (!server->url[0]) {
        set_error(error, error_size, tr("The server address is missing"));
        return -1;
    }
    struct wd_server servers[WD_MAX_SERVERS];
    int count = wd_servers_load(servers, WD_MAX_SERVERS);
    if (index < 0 && count >= WD_MAX_SERVERS) {
        set_error(error, error_size, tr("Too many saved servers"));
        return -1;
    }
    if (index >= count) {
        set_error(error, error_size, tr("The server no longer exists"));
        return -1;
    }
    struct wd_server *saved = &servers[index < 0 ? count : index];
    /* A name of its own (edited in webdav.json) survives a change. */
    char name[sizeof(saved->name)] = "";
    if (index >= 0) {
        char generated[sizeof(name)];
        default_name(saved->url, generated, sizeof(generated));
        if (strcmp(saved->name, generated) != 0)
            copy_string(name, sizeof(name), saved->name);
    }
    *saved = *server;
    /* A bare "host:port" has no scheme; libcurl needs one. */
    if (!strstr(saved->url, "://"))
        snprintf(saved->url, sizeof(saved->url), "http://%.*s",
                 (int)(sizeof(saved->url) - 8), server->url);
    if (!saved->name[0])
        copy_string(saved->name, sizeof(saved->name), name);
    if (!saved->name[0])
        default_name(saved->url, saved->name, sizeof(saved->name));
    return servers_save(servers, index < 0 ? count + 1 : count, error,
                        error_size);
}

int wd_server_remove(int index, char *error, size_t error_size)
{
    struct wd_server servers[WD_MAX_SERVERS];
    int count = wd_servers_load(servers, WD_MAX_SERVERS);
    if (index < 0 || index >= count) {
        set_error(error, error_size, tr("The server no longer exists"));
        return -1;
    }
    memmove(&servers[index], &servers[index + 1],
            (size_t)(count - index - 1) * sizeof(*servers));
    return servers_save(servers, count - 1, error, error_size);
}

int wd_server_get(int index, struct wd_server *out)
{
    struct wd_server servers[WD_MAX_SERVERS];
    int count = wd_servers_load(servers, WD_MAX_SERVERS);
    if (index < 0 || index >= count)
        return -1;
    *out = servers[index];
    return 0;
}

/* ---- URL helpers -------------------------------------------------------- */

static void base64_encode(const unsigned char *in, size_t length, char *out,
                          size_t size)
{
    static const char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t used = 0;
    for (size_t i = 0; i < length; i += 3) {
        size_t left = length - i;
        unsigned value = (unsigned)in[i] << 16;
        if (left > 1)
            value |= (unsigned)in[i + 1] << 8;
        if (left > 2)
            value |= in[i + 2];
        const char quad[4] = {
            digits[(value >> 18) & 0x3F],
            digits[(value >> 12) & 0x3F],
            left > 1 ? digits[(value >> 6) & 0x3F] : '=',
            left > 2 ? digits[value & 0x3F] : '=',
        };
        for (int k = 0; k < 4 && used + 1 < size; ++k)
            out[used++] = quad[k];
    }
    if (size)
        out[used < size ? used : size - 1] = '\0';
}

void wd_authorization_header(const struct wd_server *server, char *out,
                             size_t size)
{
    out[0] = '\0';
    if (!server->user[0])
        return;
    char pair[sizeof(server->user) + sizeof(server->pass) + 1];
    int length = snprintf(pair, sizeof(pair), "%s:%s", server->user,
                          server->pass);
    if (length <= 0)
        return;
    char encoded[((sizeof(pair) + 2) / 3) * 4 + 1];
    base64_encode((const unsigned char *)pair, (size_t)length, encoded,
                  sizeof(encoded));
    snprintf(out, size, "Authorization: Basic %s", encoded);
}

/* Start of the path in `url` (after scheme://host[:port]), NULL if none. */
static const char *url_path(const char *url)
{
    const char *scheme = strstr(url, "://");
    return strchr(scheme ? scheme + 3 : url, '/');
}

void wd_origin(const struct wd_server *server, char *out, size_t size)
{
    const char *path = url_path(server->url);
    size_t length = path ? (size_t)(path - server->url) : strlen(server->url);
    snprintf(out, size, "%.*s", (int)length, server->url);
}

void wd_base_path(const struct wd_server *server, char *out, size_t size)
{
    const char *path = url_path(server->url);
    if (!path)
        path = "/";
    size_t length = strlen(path);
    snprintf(out, size, "%s%s", path, path[length - 1] == '/' ? "" : "/");
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void wd_url_decode(const char *in, char *out, size_t size)
{
    size_t used = 0;
    for (size_t i = 0; in[i] && used + 1 < size; ++i) {
        if (in[i] == '%' && hex_value(in[i + 1]) >= 0 &&
            hex_value(in[i + 2]) >= 0) {
            out[used++] =
                (char)(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
            i += 2;
        } else {
            out[used++] = in[i];
        }
    }
    out[used] = '\0';
}

/* The five predefined XML entities ("&amp;" in an href is a plain "&"). */
static void xml_unescape(const char *in, size_t length, char *out,
                         size_t size)
{
    static const struct {
        const char *entity;
        char c;
    } entities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                    {"&quot;", '"'}, {"&apos;", '\''}};
    size_t used = 0;
    for (size_t i = 0; i < length && used + 1 < size;) {
        size_t k = 0;
        while (k < sizeof(entities) / sizeof(entities[0]) &&
               strncmp(in + i, entities[k].entity,
                       strlen(entities[k].entity)) != 0)
            ++k;
        if (k < sizeof(entities) / sizeof(entities[0])) {
            out[used++] = entities[k].c;
            i += strlen(entities[k].entity);
        } else {
            out[used++] = in[i++];
        }
    }
    out[used] = '\0';
}

/* ---- PROPFIND ----------------------------------------------------------- */

struct buffer {
    char *data;
    size_t size;
    size_t limit; /* 0: no limit */
};

static size_t write_body(char *data, size_t size, size_t count, void *user)
{
    struct buffer *buffer = user;
    size_t total = size * count;
    if (buffer->limit && buffer->size + total > buffer->limit)
        return 0; /* aborts the transfer */
    char *grown = realloc(buffer->data, buffer->size + total + 1);
    if (!grown)
        return 0;
    buffer->data = grown;
    memcpy(buffer->data + buffer->size, data, total);
    buffer->size += total;
    buffer->data[buffer->size] = '\0';
    return total;
}

/* First opening tag with local name `name` in [from, end) ("<d:href>",
 * "<D:collection/>"); returns the position just past its '>' or NULL. */
static const char *find_tag(const char *from, const char *end,
                            const char *name)
{
    size_t length = strlen(name);
    for (const char *p = from; p < end && (p = memchr(p, '<', (size_t)(end - p)));
         ++p) {
        const char *start = p + 1;
        if (start >= end || *start == '/')
            continue;
        const char *local = start, *q = start;
        while (q < end && *q != '>' && *q != '/' && *q != ' ') {
            if (*q == ':')
                local = q + 1;
            ++q;
        }
        if ((size_t)(q - local) == length &&
            strncasecmp(local, name, length) == 0) {
            const char *close = memchr(q, '>', (size_t)(end - q));
            return close ? close + 1 : NULL;
        }
    }
    return NULL;
}

static bool is_media_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot &&
           (strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".mp4") == 0);
}

/* A picture next to a video: "NAME-poster.jpg", "NAME.jpg", ... */
struct picture {
    char name[256];
    char path[1024];
    int64_t size;
};

/* Gives each title the first sidecar picture found, in the order of
 * local-thumbnail.c. */
static void match_pictures(struct wd_entry *entries, size_t count,
                           const struct picture *pictures,
                           size_t picture_count)
{
    static const char *const suffixes[] = {
        "-poster.jpg", ".jpg", "-thumb.jpg", "-fanart.jpg",
    };
    for (size_t i = 0; i < count && picture_count; ++i) {
        if (entries[i].is_dir)
            continue;
        const char *dot = strrchr(entries[i].name, '.');
        int base = dot ? (int)(dot - entries[i].name)
                       : (int)strlen(entries[i].name);
        for (size_t s = 0; s < sizeof(suffixes) / sizeof(suffixes[0]) &&
                           !entries[i].image[0];
             ++s) {
            char wanted[300];
            snprintf(wanted, sizeof(wanted), "%.*s%s", base, entries[i].name,
                     suffixes[s]);
            for (size_t p = 0; p < picture_count; ++p)
                if (strcmp(pictures[p].name, wanted) == 0) {
                    memcpy(entries[i].image, pictures[p].path,
                           sizeof(entries[i].image));
                    entries[i].image_size = pictures[p].size;
                    break;
                }
        }
    }
}

/* Decoded `path` without trailing slashes into `out`. */
static void decoded_path(const char *path, char *out, size_t size)
{
    wd_url_decode(path, out, size);
    size_t length = strlen(out);
    while (length > 1 && out[length - 1] == '/')
        out[--length] = '\0';
}

/* A request to `path` below the server's origin with its login, the
 * body collected in `response`. */
static CURL *open_request(const struct wd_server *server, const char *path,
                          struct buffer *response)
{
    char origin[512], url[1600];
    wd_origin(server, origin, sizeof(origin));
    snprintf(url, sizeof(url), "%s%s", origin, path);
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Stereonaut");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (server->user[0]) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERNAME, server->user);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, server->pass);
    }
    return curl;
}

int wd_fetch(const struct wd_server *server, const char *path, char **data,
             size_t *size)
{
    *data = NULL;
    *size = 0;
    struct buffer response = {.limit = 32U * 1024U * 1024U};
    CURL *curl = open_request(server, path, &response);
    if (!curl)
        return -1;
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status != 200 || !response.data) {
        free(response.data);
        return -1;
    }
    *data = response.data;
    *size = response.size;
    return 0;
}

int wd_list(const struct wd_server *server, const char *dir_path,
            struct wd_entry **out, size_t *count, char *error,
            size_t error_size)
{
    *out = NULL;
    *count = 0;
    struct buffer response = {0};
    CURL *curl = open_request(server, dir_path, &response);
    if (!curl) {
        set_error(error, error_size, tr("Not enough memory"));
        return -1;
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Depth: 1");
    headers = curl_slist_append(headers, "Content-Type: application/xml");
    static const char body[] =
        "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:resourcetype/><d:getcontentlength/></d:prop></d:propfind>";
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || status == 401 || status < 200 ||
        status >= 300 || !response.data) {
        if (result != CURLE_OK)
            set_error(error, error_size, curl_easy_strerror(result));
        else if (status == 401)
            set_error(error, error_size, tr("The server refused the sign-in"));
        else if (status < 200 || status >= 300)
            snprintf(error, error_size, tr("WebDAV error (HTTP %ld)"),
                     status);
        else
            set_error(error, error_size, tr("Invalid answer from the server"));
        free(response.data);
        if (result != CURLE_OK || status == 404)
            return WD_UNREACHABLE;
        return status == 401 ? WD_LOGIN_REJECTED : -1;
    }

    char self[1024];
    decoded_path(dir_path, self, sizeof(self));
    size_t capacity = 0, picture_count = 0, picture_capacity = 0;
    struct wd_entry *entries = NULL;
    struct picture *pictures = NULL;
    const char *end = response.data + response.size;
    for (const char *href = find_tag(response.data, end, "href"); href;) {
        const char *href_end = memchr(href, '<', (size_t)(end - href));
        if (!href_end)
            break;
        const char *next = find_tag(href_end, end, "href");
        const char *properties_end = next ? next : end;

        /* The path below the origin, still percent-encoded. */
        char raw[1024];
        size_t raw_length = (size_t)(href_end - href);
        xml_unescape(href, raw_length, raw, sizeof(raw));
        href = next;
        if (raw_length >= sizeof(raw))
            continue; /* too long to be shown */
        const char *path = raw;
        if (strstr(raw, "://")) {
            path = url_path(raw);
            if (!path)
                path = "/";
        }
        char decoded[1024];
        decoded_path(path, decoded, sizeof(decoded));
        if (strcmp(decoded, self) == 0)
            continue; /* the folder itself */
        const char *slash = strrchr(decoded, '/');
        const char *name = slash ? slash + 1 : decoded;
        bool is_dir = find_tag(href_end, properties_end, "collection");
        const char *dot = strrchr(name, '.');
        bool picture = !is_dir && dot && strcasecmp(dot, ".jpg") == 0;
        if (!name[0] || name[0] == '.' || /* hidden */
            (!is_dir && !picture && !is_media_name(name)))
            continue;
        /* Folders end in '/'; a path that would be cut is skipped. */
        char location[sizeof(entries->path)];
        int written = snprintf(location, sizeof(location), "%s%s", path,
                               is_dir && path[strlen(path) - 1] != '/' ? "/"
                                                                       : "");
        if (written < 0 || (size_t)written >= sizeof(location))
            continue;
        const char *length = find_tag(href_end, properties_end,
                                      "getcontentlength");
        int64_t size = length && !is_dir ? strtoll(length, NULL, 10) : 0;
        if (picture) {
            if (picture_count == picture_capacity) {
                size_t grown = picture_capacity ? picture_capacity * 2 : 16;
                struct picture *more =
                    realloc(pictures, grown * sizeof(*more));
                if (!more)
                    continue; /* the title just keeps its key frame */
                pictures = more;
                picture_capacity = grown;
            }
            struct picture *added = &pictures[picture_count++];
            snprintf(added->name, sizeof(added->name), "%s", name);
            memcpy(added->path, location, (size_t)written + 1);
            added->size = size;
            continue;
        }

        if (*count == capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            struct wd_entry *more = realloc(entries, grown * sizeof(*more));
            if (!more) {
                free(entries);
                free(pictures);
                free(response.data);
                set_error(error, error_size, tr("Not enough memory"));
                return -1;
            }
            entries = more;
            capacity = grown;
        }
        struct wd_entry *entry = &entries[(*count)++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        memcpy(entry->path, location, (size_t)written + 1);
        entry->is_dir = is_dir;
        entry->size = size;
    }
    match_pictures(entries, *count, pictures, picture_count);
    free(pictures);
    free(response.data);
    *out = entries;
    return 0;
}
