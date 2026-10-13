#ifndef STEREONAUT_I18N_H
#define STEREONAUT_I18N_H

#include <stddef.h>

/* Translations of the user interface. As with gettext, the English text in
 * the source is the key: tr("Settings") returns the text of the language in
 * effect, or the English text where the language has none. Strings in
 * static tables are marked with N_() and passed through tr() where they are
 * shown.
 *
 * A language is one i18n-CODE.c with a struct i18n_language (English/
 * translated pairs in any order) listed in i18n_languages (i18n.c). The
 * build scripts compile every i18n*.c. Format strings keep their
 * conversions in the same order. */

struct i18n_entry {
    const char *english, *text;
};

struct i18n_language {
    const char *code;  /* "de", or with region "pt_BR" */
    const char *name;  /* in the language itself: "Deutsch" */
    char decimal_point;
    /* The 44 keys of the search keyboard (four rows of eleven), separated
     * by spaces; NULL keeps the English layout. */
    const char *keyboard;
    const struct i18n_entry *entries;
    size_t count;
};

/* English first; it has no entries. */
extern const struct i18n_language *const i18n_languages[];
extern const int i18n_language_count;

#define I18N_AUTO (-1)

const char *tr(const char *english) __attribute__((format_arg(1)));
#define N_(english) (english)

/* Index of the language the system asks for: STEREONAUT_LANGUAGE, then
 * LANGUAGE/LC_ALL/LC_MESSAGES, then the language of the Steam client (on
 * SteamOS LANG stays en_US whatever the user picked), then LANG. The first
 * of them that names a language decides; without a translation for it, or
 * if none does, English (0). */
int i18n_detect(void);
/* Applies a choice: "auto" (or NULL, or an unknown code) follows
 * i18n_detect(), otherwise a language code. Returns the index in effect. */
int i18n_select(const char *choice);
/* The language in effect and the choice behind it (index or I18N_AUTO). */
int i18n_current(void);
int i18n_choice(void);
/* Replaces the '.' of printf numbers by the language's decimal point. */
void i18n_localize_number(char *text);

#endif
