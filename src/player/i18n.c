#define _POSIX_C_SOURCE 200809L
#include "i18n.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern const struct i18n_language i18n_german;

static const struct i18n_language i18n_english = {
    .code = "en",
    .name = "English",
    .decimal_point = '.',
};

const struct i18n_language *const i18n_languages[] = {
    &i18n_english,
    &i18n_german,
};

#define LANGUAGE_COUNT (sizeof(i18n_languages) / sizeof(i18n_languages[0]))
const int i18n_language_count = (int)LANGUAGE_COUNT;

static atomic_int current_language;   /* index into i18n_languages */
static atomic_int chosen_language = I18N_AUTO;

/* Entries of each language sorted by the English text (built once, shared
 * by the threads that translate). */
static const struct i18n_entry **sorted[LANGUAGE_COUNT];
static pthread_once_t sorted_once = PTHREAD_ONCE_INIT;

static int compare_entries(const void *a, const void *b)
{
    const struct i18n_entry *const *left = a, *const *right = b;
    return strcmp((*left)->english, (*right)->english);
}

static void sort_entries(void)
{
    for (size_t i = 0; i < LANGUAGE_COUNT; ++i) {
        const struct i18n_language *language = i18n_languages[i];
        if (!language->count)
            continue;
        sorted[i] = malloc(language->count * sizeof(*sorted[i]));
        if (!sorted[i])
            continue;
        for (size_t j = 0; j < language->count; ++j)
            sorted[i][j] = &language->entries[j];
        qsort(sorted[i], language->count, sizeof(*sorted[i]),
              compare_entries);
    }
}

const char *tr(const char *english)
{
    int index = atomic_load(&current_language);
    if (!english || index <= 0 || index >= (int)LANGUAGE_COUNT)
        return english;
    pthread_once(&sorted_once, sort_entries);
    if (!sorted[index])
        return english;
    const struct i18n_entry key = {english, NULL}, *wanted = &key;
    const struct i18n_entry **found =
        bsearch(&wanted, sorted[index], i18n_languages[index]->count,
                sizeof(*sorted[index]), compare_entries);
    return found && (*found)->text[0] ? (*found)->text : english;
}

/* ---- detection --------------------------------------------------------- */

enum { NOT_NAMED = -1, NOT_AVAILABLE = -2 };

/* Index of the language a locale name ("de_DE.UTF-8@euro", "pt_BR", "de")
 * asks for; NOT_NAMED for "", "C" and "POSIX", NOT_AVAILABLE for a
 * language without translation. The region decides only between two
 * translations of one language. */
static int match_locale(const char *name)
{
    if (!name || !name[0] || strcmp(name, "C") == 0 ||
        strncmp(name, "C.", 2) == 0 || strcmp(name, "POSIX") == 0)
        return NOT_NAMED;
    char wanted[16];
    size_t length = strcspn(name, ".@");
    if (length >= sizeof(wanted))
        return NOT_AVAILABLE;
    for (size_t i = 0; i < length; ++i)
        wanted[i] = name[i] == '-' ? '_' : name[i];
    wanted[length] = '\0';
    size_t primary = strcspn(wanted, "_");
    int fallback = NOT_AVAILABLE;
    for (size_t i = 0; i < LANGUAGE_COUNT; ++i) {
        const char *code = i18n_languages[i]->code;
        if (strcasecmp(code, wanted) == 0)
            return (int)i;
        if (fallback < 0 && strcspn(code, "_") == primary &&
            strncasecmp(code, wanted, primary) == 0)
            fallback = (int)i;
    }
    return fallback;
}

/* A colon-separated priority list (LANGUAGE): the first entry with a
 * translation. */
static int match_list(const char *list)
{
    int result = NOT_NAMED;
    while (list && *list) {
        size_t length = strcspn(list, ":");
        char item[32];
        if (length && length < sizeof(item)) {
            memcpy(item, list, length);
            item[length] = '\0';
            int match = match_locale(item);
            if (match >= 0)
                return match;
            if (match == NOT_AVAILABLE)
                result = NOT_AVAILABLE;
        }
        list += length;
        if (*list == ':')
            ++list;
    }
    return result;
}

/* Steam's names for its client languages. */
static const struct {
    const char *steam, *locale;
} steam_languages[] = {
    {"english", "en"},       {"german", "de"},       {"french", "fr"},
    {"italian", "it"},       {"spanish", "es"},      {"latam", "es_419"},
    {"portuguese", "pt"},    {"brazilian", "pt_BR"}, {"dutch", "nl"},
    {"danish", "da"},        {"swedish", "sv"},      {"norwegian", "nb"},
    {"finnish", "fi"},       {"polish", "pl"},       {"czech", "cs"},
    {"hungarian", "hu"},     {"romanian", "ro"},     {"bulgarian", "bg"},
    {"greek", "el"},         {"turkish", "tr"},      {"russian", "ru"},
    {"ukrainian", "uk"},     {"arabic", "ar"},       {"thai", "th"},
    {"vietnamese", "vi"},    {"indonesian", "id"},   {"japanese", "ja"},
    {"koreana", "ko"},       {"schinese", "zh_CN"},  {"tchinese", "zh_TW"},
};

/* Copies the quoted VDF string at *cursor and moves past it. */
static void vdf_string(const char **cursor, char *text, size_t size)
{
    const char *p = *cursor + 1;
    size_t used = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            ++p;
        if (used + 1 < size)
            text[used++] = *p;
        ++p;
    }
    text[used] = '\0';
    *cursor = *p ? p + 1 : p;
}

/* The client language from Steam's registry.vdf
 * (Registry/HKCU/Software/Valve/Steam/language). */
static bool steam_language(char *language, size_t size)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    static const char *const files[] = {
        ".steam/registry.vdf",
        ".var/app/com.valvesoftware.Steam/.steam/registry.vdf",
    };
    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); ++f) {
        char path[4200];
        snprintf(path, sizeof(path), "%s/%s", home, files[f]);
        FILE *file = fopen(path, "rb");
        if (!file)
            continue;
        enum { TEXT_MAX = 1 << 16 };
        char *text = malloc(TEXT_MAX);
        size_t length = text ? fread(text, 1, TEXT_MAX - 1, file) : 0;
        fclose(file);
        if (!text)
            return false;
        text[length] = '\0';
        char sections[8][64] = {{0}}, key[64] = "", token[64];
        bool have_key = false;
        int depth = 0;
        language[0] = '\0';
        for (const char *p = text; *p;) {
            if (*p == '{') {
                if (depth < 8)
                    snprintf(sections[depth], sizeof(sections[depth]), "%s",
                             have_key ? key : "");
                ++depth;
                have_key = false;
                ++p;
            } else if (*p == '}') {
                if (depth > 0)
                    --depth;
                have_key = false;
                ++p;
            } else if (*p == '"') {
                vdf_string(&p, token, sizeof(token));
                if (!have_key) {
                    snprintf(key, sizeof(key), "%s", token);
                    have_key = true;
                    continue;
                }
                have_key = false;
                if (strcasecmp(key, "language") != 0 || !token[0])
                    continue;
                bool steam = depth > 0 && depth <= 8 &&
                             strcasecmp(sections[depth - 1], "Steam") == 0;
                if (steam || !language[0])
                    snprintf(language, size, "%s", token);
                if (steam)
                    break;
            } else {
                ++p;
            }
        }
        free(text);
        if (language[0])
            return true;
    }
    return false;
}

static int match_steam(void)
{
    char language[64];
    if (!steam_language(language, sizeof(language)))
        return NOT_NAMED;
    for (size_t i = 0;
         i < sizeof(steam_languages) / sizeof(steam_languages[0]); ++i)
        if (strcasecmp(language, steam_languages[i].steam) == 0)
            return match_locale(steam_languages[i].locale);
    return NOT_AVAILABLE;
}

static const char *environment(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] ? value : NULL;
}

int i18n_detect(void)
{
    int sources[] = {
        match_locale(environment("STEREONAUT_LANGUAGE")),
        match_list(environment("LANGUAGE")),
        match_locale(environment("LC_ALL")),
        match_locale(environment("LC_MESSAGES")),
        NOT_NAMED, /* Steam, read only when needed */
        match_locale(environment("LANG")),
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        int match = i == 4 ? match_steam() : sources[i];
        if (match != NOT_NAMED)
            return match >= 0 ? match : 0;
    }
    return 0;
}

int i18n_select(const char *choice)
{
    int chosen = I18N_AUTO;
    for (size_t i = 0; choice && i < LANGUAGE_COUNT; ++i)
        if (strcmp(choice, i18n_languages[i]->code) == 0)
            chosen = (int)i;
    int index = chosen == I18N_AUTO ? i18n_detect() : chosen;
    atomic_store(&chosen_language, chosen);
    atomic_store(&current_language, index);
    return index;
}

int i18n_current(void)
{
    return atomic_load(&current_language);
}

int i18n_choice(void)
{
    return atomic_load(&chosen_language);
}

void i18n_localize_number(char *text)
{
    char point = i18n_languages[i18n_current()]->decimal_point;
    if (point && point != '.')
        for (char *c = text; *c; ++c)
            if (*c == '.')
                *c = point;
}
