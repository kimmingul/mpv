/*
 * NanumPlayer: interface language of the native application.
 *
 * Six languages (ko, en, ja, zh-CN, zh-TW, es) in the order of the shared
 * translation catalogs (locales/native.json). The choice is persisted in
 * <settings dir>/ui-language.conf (one canonical code, or "auto", plus a
 * newline), published as user-data/nanum/ui-language (resolved code) and
 * user-data/nanum/ui-language-mode (the selection: "auto" or a code), and
 * changed at run time by the command "nanum-ui-language".
 *
 * The interface language is independent of the languages the AI reads and
 * writes (nanum-language, nanum-target-language).
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_I18N_H_
#define MP_NANUM_I18N_H_

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

struct MPContext;

// Index = position in the catalogs.
enum nanum_ui_lang {
    NANUM_UI_KO, NANUM_UI_EN, NANUM_UI_JA, NANUM_UI_ZH_CN, NANUM_UI_ZH_TW, NANUM_UI_ES,
    NANUM_UI_COUNT
};

// Resolved language right now. Thread-safe, allocation-free.
unsigned nanum_ui_language(void);
const char *nanum_ui_language_code(unsigned language);
// Increases whenever the language changed (threads that cache localized text compare it).
unsigned nanum_ui_language_generation(void);

// Maps a language tag (BCP 47 / Windows / POSIX form: "zh-Hant-TW", "ko_KR",
// "es-MX", "zh-CN", ...) to the supported language; -1 if unsupported.
int nanum_ui_language_parse(const char *tag);

// Marks a catalog key (a string literal, adjacent literals allowed) that a function
// translates itself (a printf-style wrapper that calls nanum_tr() on its format). It
// changes nothing; tools read it to find the keys.
#define N_(s) (s)

// Localized text of a source string: the source itself when the catalog has no entry. The
// text has the same printf conversions as the key, so a format string may be translated:
// snprintf(buf, sizeof(buf), nanum_tr("%s cannot be read"), path).
__attribute__((format_arg(1))) const char *nanum_tr(const char *key);
__attribute__((format_arg(1))) const char *nanum_tr_lang(const char *key, unsigned language);
const wchar_t *nanum_trw(const wchar_t *key);

// Messages that stay translatable after they were stored (status text shown later, maybe
// after a language change). Stored form: "\x02" key ("\x1f" argument)*. An argument
// starting with "\x03" is itself a catalog key. Text that does not start with "\x02" is
// raw (an external diagnostic) and passes through unchanged. Placeholders in the key are
// "%s" (substituted by the arguments in order) and "%%".
// Encodes the message into buf; when it does not fit, the text of the current language is
// stored instead. The arguments are const char *, terminated by NULL.
void nanum_msg(char *buf, size_t size, const char *key, ...);
#define NANUM_MSG(buf, key, ...) nanum_msg((buf), sizeof(buf), (key), ##__VA_ARGS__, (const char *)NULL)
// Same, as the marker form of one catalog key to use as an argument: "\x03" key.
#define NANUM_ARG(key) ("\x03" key)
// Renders a stored message into out (NUL-terminated, never cut inside a UTF-8 sequence).
void nanum_msg_render(const char *stored, char *out, size_t size);
void nanum_msg_render_lang(const char *stored, char *out, size_t size, unsigned language);

// Loads the language preference (override: the --nanum-ui-language value, NULL or "" if
// not given; settings_dir: where ui-language.conf lives, may be NULL) and publishes it.
// Called once before the scripts load.
void nanum_ui_language_startup(struct MPContext *mpctx, const char *settings_dir,
                               const char *override);
// The command "nanum-ui-language <auto|code>": applies, publishes and persists the
// selection. Returns false (with error text in err) when the value is unknown; a failed
// write is reported in err too, but the language is still applied for this session
// ("persisted" is then false).
bool nanum_ui_language_command(struct MPContext *mpctx, const char *value, bool *persisted,
                               char *err, size_t err_size);

#endif
