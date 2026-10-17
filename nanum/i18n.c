/*
 * NanumPlayer: interface language of the native application. See i18n.h.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "mpv/client.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/path_utils.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/m_property.h"
#include "osdep/io.h"
#include "osdep/nanum-accel.h"
#include "osdep/nanum-diag.h"
#include "osdep/nanum-models.h"
#include "osdep/nanum-youtube.h"
#include "player/command.h"
#include "player/core.h"
#include "nanum/i18n.h"

// Generated from locales/native.json by tools/compile_translations.py (see meson.build).
#include "nanum_native_strings.h"
#include "nanum_native_strings_w.h"

#define CONF_NAME "ui-language.conf"
#define MSG_MARK '\x02'
#define MSG_SEP '\x1f'
#define ARG_KEY '\x03'
#define MSG_MAX_ARGS 8

// --nanum-ui-language=<auto|ko|en|ja|zh-CN|zh-TW|es>: overrides the saved choice for this
// run (isolated tests). Empty: the saved choice / the system language decides.
#define OPT_BASE_STRUCT struct nanum_i18n_opts
struct nanum_i18n_opts {
    char *ui_language;
};

const struct m_sub_options nanum_i18n_conf = {
    .opts = (const m_option_t[]) {
        {"nanum-ui-language", OPT_STRING(ui_language)},
        {0}
    },
    .size = sizeof(struct nanum_i18n_opts),
    .defaults = &(const struct nanum_i18n_opts){
        .ui_language = "",
    },
};

static const char *const codes[NANUM_UI_COUNT] = {"ko", "en", "ja", "zh-CN", "zh-TW", "es"};

static atomic_uint cur_language = UINT_MAX;     // UINT_MAX: not resolved yet
static atomic_uint cur_generation;
// Written by the startup and the command (the core thread) only.
static char cur_mode[16] = "auto";
static char conf_dir[1024];

const char *nanum_ui_language_code(unsigned language)
{
    return codes[language < NANUM_UI_COUNT ? language : NANUM_UI_EN];
}

int nanum_ui_language_parse(const char *tag)
{
    if (!tag)
        return -1;
    char low[40];
    size_t n = 0;
    for (; tag[n] && n < sizeof(low) - 1; n++)
        low[n] = tag[n] == '_' ? '-' : (char)tolower((unsigned char)tag[n]);
    low[n] = 0;
    if (tag[n])
        return -1;      // too long to be a language tag

    char *sub[8];
    int num = 0;
    for (char *p = low; p && num < MP_ARRAY_SIZE(sub); ) {
        sub[num++] = p;
        p = strchr(p, '-');
        if (p)
            *p++ = 0;
    }
    if (!strcmp(sub[0], "ko"))
        return NANUM_UI_KO;
    if (!strcmp(sub[0], "en"))
        return NANUM_UI_EN;
    if (!strcmp(sub[0], "ja"))
        return NANUM_UI_JA;
    if (!strcmp(sub[0], "es"))
        return NANUM_UI_ES;
    if (!strcmp(sub[0], "zh")) {
        // Traditional script or the regions that write it; everything else is Simplified.
        for (int i = 1; i < num; i++) {
            if (!strcmp(sub[i], "hant") || !strcmp(sub[i], "tw") || !strcmp(sub[i], "hk") ||
                !strcmp(sub[i], "mo") || !strcmp(sub[i], "cht"))
                return NANUM_UI_ZH_TW;
        }
        return NANUM_UI_ZH_CN;
    }
    return -1;
}

// The language of the operating system: the user's Windows display language, "en" when
// that one is not supported.
static unsigned system_language(void)
{
    int found = -1;
#ifdef _WIN32
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, NULL, &size) && size) {
        wchar_t *list = calloc(size, sizeof(wchar_t));
        if (list && GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list, &size) &&
            count > 0)
        {
            char tag[64];
            if (WideCharToMultiByte(CP_UTF8, 0, list, -1, tag, sizeof(tag), NULL, NULL))
                found = nanum_ui_language_parse(tag);
        }
        free(list);
    }
    if (found < 0 && !count) {
        wchar_t name[LOCALE_NAME_MAX_LENGTH];
        char tag[64];
        if (LCIDToLocaleName(GetUserDefaultUILanguage(), name, MP_ARRAY_SIZE(name), 0) &&
            WideCharToMultiByte(CP_UTF8, 0, name, -1, tag, sizeof(tag), NULL, NULL))
            found = nanum_ui_language_parse(tag);
    }
#else
    static const char *const vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    for (int i = 0; i < MP_ARRAY_SIZE(vars) && found < 0; i++) {
        const char *v = getenv(vars[i]);
        if (v && v[0]) {
            char tag[40];
            snprintf(tag, sizeof(tag), "%s", v);
            tag[strcspn(tag, ".@")] = 0;
            found = nanum_ui_language_parse(tag);
            break;
        }
    }
#endif
    return found >= 0 ? (unsigned)found : NANUM_UI_EN;
}

unsigned nanum_ui_language(void)
{
    unsigned l = atomic_load(&cur_language);
    if (l == UINT_MAX) {
        unsigned sys = system_language();
        if (atomic_compare_exchange_strong(&cur_language, &l, sys))
            l = sys;
    }
    return l;
}

unsigned nanum_ui_language_generation(void)
{
    return atomic_load(&cur_generation);
}

const char *nanum_tr_lang(const char *key, unsigned language)
{
    return nanum_native_tr(key, language);
}

const char *nanum_tr(const char *key)
{
    return nanum_native_tr(key, nanum_ui_language());
}

const wchar_t *nanum_trw(const wchar_t *key)
{
    return nanum_native_w_tr(key, nanum_ui_language());
}

// ---------------------------------------------------------------------------
// Stored messages
// ---------------------------------------------------------------------------

// Copies text into out without ever leaving a cut UTF-8 sequence at its end.
static void copy_utf8(char *out, size_t size, const char *text, size_t len)
{
    if (!size)
        return;
    if (len > size - 1)
        len = size - 1;
    if (len) {
        // The input may itself end at a bounded intermediate buffer, not just
        // at this output's limit. Drop an incomplete final code point either way.
        size_t first = len - 1;
        while (first && ((unsigned char)text[first] & 0xC0) == 0x80)
            first--;
        unsigned char lead = text[first];
        size_t width = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (len - first < width)
            len = first;
    }
    memcpy(out, text, len);
    out[len] = 0;
}

void nanum_msg_render_lang(const char *stored, char *out, size_t size, unsigned language)
{
    if (!size)
        return;
    if (!stored || stored[0] != MSG_MARK) {
        copy_utf8(out, size, stored ? stored : "", stored ? strlen(stored) : 0);
        return;
    }
    const char *p = stored + 1;
    size_t keylen = strcspn(p, (char[]){MSG_SEP, 0});
    char key[512];
    if (keylen >= sizeof(key)) {
        copy_utf8(out, size, p, keylen);
        return;
    }
    memcpy(key, p, keylen);
    key[keylen] = 0;
    p += keylen;

    const char *args[MSG_MAX_ARGS];
    size_t arglen[MSG_MAX_ARGS];
    int num = 0;
    while (*p == MSG_SEP && num < MSG_MAX_ARGS) {
        p++;
        args[num] = p;
        arglen[num] = strcspn(p, (char[]){MSG_SEP, 0});
        p += arglen[num++];
    }

    char tmp[2048];
    size_t used = 0;
    const char *t = nanum_tr_lang(key, language);
    int next = 0;
#define PUT(s, n) do { size_t n_ = (n); if (n_ > sizeof(tmp) - 1 - used) n_ = sizeof(tmp) - 1 - used; \
                       memcpy(tmp + used, (s), n_); used += n_; } while (0)
    for (; *t; t++) {
        if (t[0] == '%' && t[1] == '%') {
            PUT("%", 1);
            t++;
        } else if (t[0] == '%' && t[1] == 's') {
            if (next < num) {
                const char *a = args[next];
                size_t al = arglen[next++];
                if (al && a[0] == ARG_KEY) {
                    char ak[256];
                    copy_utf8(ak, sizeof(ak), a + 1, al - 1);
                    const char *tr = nanum_tr_lang(ak, language);
                    PUT(tr, strlen(tr));
                } else {
                    PUT(a, al);
                }
            }
            t++;
        } else {
            PUT(t, 1);
        }
    }
#undef PUT
    tmp[used] = 0;
    copy_utf8(out, size, tmp, used);
}

void nanum_msg_render(const char *stored, char *out, size_t size)
{
    nanum_msg_render_lang(stored, out, size, nanum_ui_language());
}

void nanum_msg(char *buf, size_t size, const char *key, ...)
{
    if (!size)
        return;
    const char *args[MSG_MAX_ARGS];
    int num = 0;
    size_t need = 1 + strlen(key);
    va_list ap;
    va_start(ap, key);
    for (const char *a; (a = va_arg(ap, const char *)) && num < MSG_MAX_ARGS; ) {
        args[num++] = a;
        need += 1 + strlen(a);
    }
    va_end(ap);

    bool clean = !strchr(key, MSG_SEP);
    for (int i = 0; i < num; i++)
        clean &= !strchr(args[i], MSG_SEP);
    if (need >= size || !clean) {
        // Cannot be kept re-translatable: store the current language's text.
        char text[2048];
        char enc[2048];
        size_t pos = 0;
        enc[pos++] = MSG_MARK;
        for (const char *k = key; *k && pos < sizeof(enc) - 1; k++)
            enc[pos++] = *k == MSG_SEP ? ' ' : *k;
        for (int i = 0; i < num; i++) {
            if (pos < sizeof(enc) - 1)
                enc[pos++] = MSG_SEP;
            for (const char *a = args[i]; *a && pos < sizeof(enc) - 1; a++)
                enc[pos++] = *a == MSG_SEP ? ' ' : *a;
        }
        enc[pos] = 0;
        nanum_msg_render(enc, text, sizeof(text));
        copy_utf8(buf, size, text, strlen(text));
        return;
    }
    char *w = buf;
    *w++ = MSG_MARK;
    memcpy(w, key, strlen(key));
    w += strlen(key);
    for (int i = 0; i < num; i++) {
        *w++ = MSG_SEP;
        memcpy(w, args[i], strlen(args[i]));
        w += strlen(args[i]);
    }
    *w = 0;
}

// ---------------------------------------------------------------------------
// Preference
// ---------------------------------------------------------------------------

static void publish_string(struct MPContext *mpctx, const char *name, const char *value)
{
    mpv_node node = {.format = MPV_FORMAT_STRING, .u.string = (char *)value};
    mp_property_do(name, M_PROPERTY_SET_NODE, &node, mpctx);
}

static void publish(struct MPContext *mpctx, const char *error)
{
    publish_string(mpctx, "user-data/nanum/ui-language",
                   nanum_ui_language_code(nanum_ui_language()));
    publish_string(mpctx, "user-data/nanum/ui-language-mode", cur_mode);
    publish_string(mpctx, "user-data/nanum/ui-language-error", error ? error : "");
}

// "auto" or a canonical code for any accepted spelling; false if unknown.
static bool normalize_mode(const char *value, char out[16])
{
    if (!value)
        return false;
    char word[16];
    size_t n = 0;
    while (*value && isspace((unsigned char)*value))
        value++;
    for (; value[n] && !isspace((unsigned char)value[n]) && n < sizeof(word) - 1; n++)
        word[n] = value[n];
    word[n] = 0;
    const char *rest = value + n;
    while (*rest && isspace((unsigned char)*rest))
        rest++;
    if (!n || *rest)
        return false;
    if (!strcasecmp(word, "auto")) {
        snprintf(out, 16, "auto");
        return true;
    }
    int l = nanum_ui_language_parse(word);
    if (l < 0)
        return false;
    snprintf(out, 16, "%s", codes[l]);
    return true;
}

static char *conf_path(void *ctx)
{
    return conf_dir[0] ? talloc_asprintf(ctx, "%s/%s", conf_dir, CONF_NAME) : NULL;
}

static bool read_conf(char mode[16])
{
    void *tmp = talloc_new(NULL);
    char *path = conf_path(tmp);
    bool ok = false;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) {
        char line[64] = "";
        if (fgets(line, sizeof(line), f)) {
            const char *s = line;
            if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
                (unsigned char)s[2] == 0xBF)
                s += 3;
            ok = normalize_mode(s, mode);
        }
        fclose(f);
    }
    talloc_free(tmp);
    return ok;
}

// Writes the preference atomically. On failure returns the reason in why.
static bool write_conf(const char *mode, char *why, size_t why_size)
{
    void *tmp = talloc_new(NULL);
    char *path = conf_path(tmp);
    bool ok = false;
    if (!path) {
        snprintf(why, why_size, "%s", nanum_tr("설정 폴더를 찾을 수 없습니다"));
        talloc_free(tmp);
        return false;
    }
    mp_mkdirp(conf_dir);
    char *part = talloc_asprintf(tmp, "%s.tmp", path);
    FILE *f = fopen(part, "wb");
    if (!f) {
        snprintf(why, why_size, "%s", strerror(errno));
    } else {
        bool wrote = fprintf(f, "%s\n", mode) > 0;
        wrote &= fflush(f) == 0;
        int err = errno;
        wrote &= !ferror(f);
        wrote &= fclose(f) == 0;
        if (!wrote) {
            snprintf(why, why_size, "%s", strerror(err));
            unlink(part);
        } else if (rename(part, path) != 0) {
            snprintf(why, why_size, "%s", strerror(errno));
            unlink(part);
        } else {
            ok = true;
        }
    }
    talloc_free(tmp);
    return ok;
}

static void apply_mode(const char *mode)
{
    unsigned l = strcmp(mode, "auto") ? (unsigned)nanum_ui_language_parse(mode)
                                      : system_language();
    snprintf(cur_mode, sizeof(cur_mode), "%s", mode);
    unsigned old = atomic_exchange(&cur_language, l);
    if (old != l)
        atomic_fetch_add(&cur_generation, 1);
}

void nanum_ui_language_startup(struct MPContext *mpctx, const char *settings_dir,
                               const char *override)
{
    snprintf(conf_dir, sizeof(conf_dir), "%s", settings_dir ? settings_dir : "");
    char mode[16] = "auto";
    // The command line / mpv.conf override (isolated runs) beats the saved choice.
    if (override && override[0]) {
        if (!normalize_mode(override, mode)) {
            MP_WARN(mpctx, "Unknown --nanum-ui-language '%s', using the saved choice.\n",
                    override);
            override = NULL;
        }
    }
    if (!override || !override[0])
        if (!read_conf(mode))
            snprintf(mode, sizeof(mode), "auto");
    apply_mode(mode);
    publish(mpctx, "");
}

bool nanum_ui_language_command(struct MPContext *mpctx, const char *value, bool *persisted,
                               char *err, size_t err_size)
{
    *persisted = false;
    char mode[16];
    if (!normalize_mode(value, mode)) {
        snprintf(err, err_size, nanum_tr("알 수 없는 표시 언어입니다: %s"), value ? value : "");
        return false;
    }
    apply_mode(mode);

    char why[256] = "";
    char error[512] = "";
    *persisted = write_conf(mode, why, sizeof(why));
    if (!*persisted) {
        snprintf(error, sizeof(error), nanum_tr("표시 언어를 저장하지 못했습니다: %s"), why);
        MP_ERR(mpctx, "Cannot save the interface language: %s\n", why);
        snprintf(err, err_size, "%s", error);
    }
    publish(mpctx, error);

    // Native state that holds localized text is published again.
    nanum_models_refresh(mpctx);
    nanum_accel_refresh(mpctx);
    nanum_youtube_publish(mpctx);
    nanum_diag_publish(mpctx);
    return *persisted;
}
