/*
 * NanumPlayer: shared download engine (Windows). See nanum-download.h.
 *
 * HTTPS only (WinHTTP, certificate validation never relaxed, redirects may not
 * downgrade to HTTP), SHA-256 through Windows CNG, files written to a private
 * .part and moved into place only after size and digest match the catalog.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mpv_talloc.h"
#include "common/common.h"
#include "misc/node.h"
#include "osdep/io.h"
#include "osdep/nanum-download.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

#define MAX_ITEM_SIZE (INT64_C(16) << 30)
#define DISK_HEADROOM (INT64_C(64) << 20)
#define USER_AGENT L"NanumPlayer/1.2 (component download)"

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static wchar_t *W(void *ctx, const char *s)
{
    return mp_from_utf8(ctx, s);
}

char *nanum_dl_join(void *ctx, const char *a, const char *b)
{
    return talloc_asprintf(ctx, "%s\\%s", a, b);
}

void nanum_dl_trim_slashes(char *s)
{
    size_t len = strlen(s);
    while (len > 3 && (s[len - 1] == '\\' || s[len - 1] == '/'))
        s[--len] = 0;
}

bool nanum_dl_same_path(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a == '/' ? '\\' : *a, y = *b == '/' ? '\\' : *b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y)
            return false;
        if (!x)
            return true;
    }
}

bool nanum_dl_stat(const char *path, struct nanum_dl_fst *out)
{
    *out = (struct nanum_dl_fst){0};
    void *tmp = talloc_new(NULL);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(W(tmp, path), GetFileExInfoStandard, &d) &&
        !(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        out->exists = true;
        out->size = ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        out->mtime = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) |
                     d.ftLastWriteTime.dwLowDateTime;
    }
    talloc_free(tmp);
    return out->exists;
}

void nanum_dl_error_text(char *out, size_t sz, DWORD code)
{
    wchar_t buf[512] = L"";
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    if (winhttp && code >= 12000 && code < 12200)
        flags |= FORMAT_MESSAGE_FROM_HMODULE;
    DWORD n = FormatMessageW(flags, winhttp, code, 0, buf, MP_ARRAY_SIZE(buf), NULL);
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' '))
        buf[--n] = 0;
    char text[900] = "";
    if (n)
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, text, sizeof(text) - 1, NULL, NULL);
    snprintf(out, sz, "%s (error %lu)", text[0] ? text : "unknown error", (unsigned long)code);
}

static void narrow(const wchar_t *w, size_t wlen, char *out, size_t sz)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, out, (int)sz - 1, NULL, NULL);
    out[n > 0 ? n : 0] = 0;
}

static void to_hex(const uint8_t *d, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[d[i] >> 4];
        out[2 * i + 1] = H[d[i] & 15];
    }
    out[2 * n] = 0;
}

void nanum_dl_fail(struct nanum_dl *dl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl->err, sizeof(dl->err), fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------
// Catalog helpers
// ---------------------------------------------------------------------------

char *nanum_dl_read_text(void *ctx, const char *path)
{
    wchar_t *wpath = W(ctx, path);
    HANDLE f = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    LARGE_INTEGER size;
    char *text = NULL;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= (1 << 20)) {
        text = talloc_array(ctx, char, size.QuadPart + 1);
        DWORD got = 0;
        if (!ReadFile(f, text, (DWORD)size.QuadPart, &got, NULL) || got != size.QuadPart) {
            talloc_free(text);
            text = NULL;
        } else {
            text[got] = 0;
        }
    }
    CloseHandle(f);
    return text;
}

const char *nanum_dl_node_str(mpv_node *m, const char *key)
{
    mpv_node *n = node_map_get(m, key);
    return n && n->format == MPV_FORMAT_STRING ? n->u.string : NULL;
}

bool nanum_dl_node_int(mpv_node *m, const char *key, int64_t *out)
{
    mpv_node *n = node_map_get(m, key);
    if (!n)
        return false;
    if (n->format == MPV_FORMAT_INT64)
        *out = n->u.int64;
    else if (n->format == MPV_FORMAT_DOUBLE && n->u.double_ == (double)(int64_t)n->u.double_)
        *out = (int64_t)n->u.double_;
    else
        return false;
    return true;
}

static bool valid_filename(const char *s)
{
    size_t len = strlen(s);
    if (!len || len >= sizeof(((struct nanum_dl_item *)0)->file) || s[0] == '.')
        return false;
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || strchr("\\/:*?\"<>|", *s))
            return false;
    }
    return true;
}

static bool valid_sha(const char *s, char *out)
{
    if (strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'F')
            c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        out[i] = c;
    }
    out[64] = 0;
    return true;
}

bool nanum_dl_parse_item(mpv_node *m, const char *fallback_label,
                         struct nanum_dl_item *out, char *err, size_t errsz)
{
    *out = (struct nanum_dl_item){0};
    const char *id = nanum_dl_node_str(m, "id");
    const char *file = nanum_dl_node_str(m, "filename");
    const char *sha = nanum_dl_node_str(m, "sha256");
    const char *url = nanum_dl_node_str(m, "url");
    const char *label = nanum_dl_node_str(m, "label");
    int64_t size = 0;
    if (!file || !valid_filename(file) || !sha || !valid_sha(sha, out->sha) ||
        !url || strlen(url) >= NANUM_DL_MAX_URL || _strnicmp(url, "https://", 8) ||
        !nanum_dl_node_int(m, "size", &size) || size <= 0 || size > MAX_ITEM_SIZE)
    {
        snprintf(err, errsz, "catalog entry '%s' is invalid", id ? id : "?");
        *out = (struct nanum_dl_item){0};
        return false;
    }
    out->valid = true;
    out->size = size;
    snprintf(out->file, sizeof(out->file), "%s", file);
    snprintf(out->url, sizeof(out->url), "%s", url);
    snprintf(out->label, sizeof(out->label), "%s",
             label && label[0] ? label : fallback_label);
    return true;
}

// ---------------------------------------------------------------------------
// Job control
// ---------------------------------------------------------------------------

void nanum_dl_init(struct nanum_dl *dl, void *user,
                   void (*on_phase)(struct nanum_dl *, enum nanum_dl_phase, int64_t, int64_t),
                   void (*on_progress)(struct nanum_dl *, int64_t))
{
    *dl = (struct nanum_dl){0};
    dl->user = user;
    dl->on_phase = on_phase;
    dl->on_progress = on_progress;
    mp_mutex_init(&dl->lock);
}

bool nanum_dl_begin(struct nanum_dl *dl)
{
    atomic_store(&dl->cancel, false);
    dl->err[0] = 0;
    dl->buf = malloc(NANUM_DL_IO_BUF);
    return dl->buf != NULL;
}

void nanum_dl_end(struct nanum_dl *dl)
{
    free(dl->buf);
    dl->buf = NULL;
}

bool nanum_dl_cancelled(struct nanum_dl *dl)
{
    return atomic_load(&dl->cancel);
}

// Closes the in-flight request, which aborts a blocking WinHTTP call of the
// worker. The handle is owned by whoever swaps it out of dl->req first.
static void close_request(struct nanum_dl *dl)
{
    mp_mutex_lock(&dl->lock);
    HINTERNET r = dl->req;
    dl->req = NULL;
    mp_mutex_unlock(&dl->lock);
    if (r)
        WinHttpCloseHandle(r);
}

void nanum_dl_abort(struct nanum_dl *dl)
{
    atomic_store(&dl->cancel, true);
    close_request(dl);
}

// false if a cancel is pending (the caller keeps ownership and closes it).
static bool register_request(struct nanum_dl *dl, HINTERNET req)
{
    mp_mutex_lock(&dl->lock);
    bool ok = !nanum_dl_cancelled(dl);
    if (ok)
        dl->req = req;
    mp_mutex_unlock(&dl->lock);
    return ok;
}

static void phase(struct nanum_dl *dl, enum nanum_dl_phase ph, int64_t recv, int64_t total)
{
    if (dl->on_phase)
        dl->on_phase(dl, ph, recv, total);
}

static void progress(struct nanum_dl *dl, int64_t recv)
{
    if (dl->on_progress)
        dl->on_progress(dl, recv);
}

// ---------------------------------------------------------------------------
// SHA-256 (Windows CNG)
// ---------------------------------------------------------------------------

struct sha {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    uint8_t *obj;
};

static void sha_free(struct sha *s)
{
    if (s->h)
        BCryptDestroyHash(s->h);
    if (s->alg)
        BCryptCloseAlgorithmProvider(s->alg, 0);
    free(s->obj);
    *s = (struct sha){0};
}

static bool sha_begin(struct sha *s)
{
    *s = (struct sha){0};
    ULONG len = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptGetProperty(s->alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&len, sizeof(len), &got, 0) < 0 ||
        !(s->obj = malloc(len)) ||
        BCryptCreateHash(s->alg, &s->h, s->obj, len, NULL, 0, 0) < 0)
    {
        sha_free(s);
        return false;
    }
    return true;
}

static bool sha_update(struct sha *s, const void *data, DWORD n)
{
    return BCryptHashData(s->h, (PUCHAR)data, n, 0) >= 0;
}

static bool sha_finish(struct sha *s, char hex[65])
{
    uint8_t d[32];
    bool ok = BCryptFinishHash(s->h, d, sizeof(d), 0) >= 0;
    if (ok)
        to_hex(d, sizeof(d), hex);
    sha_free(s);
    return ok;
}

enum nanum_dl_res nanum_dl_hash(struct nanum_dl *dl, const struct nanum_dl_item *item,
                                const char *path, bool *match)
{
    *match = false;
    void *tmp = talloc_new(NULL);
    HANDLE f = CreateFileW(W(tmp, path), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        nanum_dl_fail(dl, "Cannot read %s: %s", path, why);
        talloc_free(tmp);
        return NANUM_DL_FAIL;
    }
    talloc_free(tmp);

    struct sha sha;
    if (!sha_begin(&sha)) {
        CloseHandle(f);
        nanum_dl_fail(dl, "SHA-256 is not available (Windows CNG failed)");
        return NANUM_DL_FAIL;
    }
    phase(dl, NANUM_DL_VERIFYING, 0, item->size);

    enum nanum_dl_res r = NANUM_DL_OK;
    int64_t total = 0;
    for (;;) {
        if (nanum_dl_cancelled(dl)) {
            r = NANUM_DL_CANCEL;
            break;
        }
        DWORD got = 0;
        if (!ReadFile(f, dl->buf, NANUM_DL_IO_BUF, &got, NULL)) {
            char why[1024];
            nanum_dl_error_text(why, sizeof(why), GetLastError());
            nanum_dl_fail(dl, "Cannot read %s: %s", path, why);
            r = NANUM_DL_FAIL;
            break;
        }
        if (!got)
            break;
        total += got;
        sha_update(&sha, dl->buf, got);
        progress(dl, total);
    }
    CloseHandle(f);
    if (r != NANUM_DL_OK) {
        sha_free(&sha);
        return r;
    }
    char hex[65] = "";
    if (!sha_finish(&sha, hex)) {
        nanum_dl_fail(dl, "SHA-256 computation failed (Windows CNG)");
        return NANUM_DL_FAIL;
    }
    *match = total == item->size && !strcmp(hex, item->sha);
    return NANUM_DL_OK;
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

static enum nanum_dl_res net_fail(struct nanum_dl *dl, const char *host, const char *what)
{
    if (nanum_dl_cancelled(dl))
        return NANUM_DL_CANCEL;
    DWORD code = GetLastError();
    char why[1024];
    nanum_dl_error_text(why, sizeof(why), code);
    const char *hint = "";
    switch (code) {
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        hint = " The TLS certificate of the server was rejected; the download was refused.";
        break;
    case ERROR_WINHTTP_TIMEOUT:
        hint = " The server did not answer in time.";
        break;
    }
    nanum_dl_fail(dl, "Network error while %s (%s): %s.%s", what, host, why, hint);
    return NANUM_DL_FAIL;
}

enum nanum_dl_res nanum_dl_fetch(struct nanum_dl *dl, const struct nanum_dl_item *item,
                                 const char *part)
{
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r = NANUM_DL_FAIL;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    struct sha sha = {0};
    char host[260] = "";

    wchar_t *wurl = W(tmp, item->url);
    wchar_t whost[256], wpath[2048], wextra[2048];
    URL_COMPONENTS uc = {
        .dwStructSize = sizeof(uc),
        .lpszHostName = whost, .dwHostNameLength = MP_ARRAY_SIZE(whost),
        .lpszUrlPath = wpath, .dwUrlPathLength = MP_ARRAY_SIZE(wpath),
        .lpszExtraInfo = wextra, .dwExtraInfoLength = MP_ARRAY_SIZE(wextra),
    };
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        nanum_dl_fail(dl, "The catalog URL of %s is not a valid HTTPS URL", item->file);
        goto out;
    }
    narrow(whost, uc.dwHostNameLength, host, sizeof(host));
    size_t plen = uc.dwUrlPathLength + uc.dwExtraInfoLength;
    wchar_t *wobj = talloc_array(tmp, wchar_t, plen + 1);
    wmemcpy(wobj, wpath, uc.dwUrlPathLength);
    wmemcpy(wobj + uc.dwUrlPathLength, wextra, uc.dwExtraInfoLength);
    wobj[plen] = 0;
    whost[uc.dwHostNameLength] = 0;

    // Destination file, exclusive: this process owns the .part.
    file = CreateFileW(W(tmp, part), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        nanum_dl_fail(dl, "Cannot create %s: %s", part, why);
        goto out;
    }

    phase(dl, NANUM_DL_CHECKING, 0, item->size);

    session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r = net_fail(dl, host, "starting the HTTP session");
        goto out;
    }
    WinHttpSetTimeouts(session, 30000, 30000, 30000, 60000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    conn = WinHttpConnect(session, whost, uc.nPort, 0);
    if (!conn) {
        r = net_fail(dl, host, "connecting");
        goto out;
    }
    req = WinHttpOpenRequest(conn, L"GET", wobj, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        r = net_fail(dl, host, "preparing the request");
        goto out;
    }
    // Redirects (HuggingFace and GitHub answer with a signed CDN URL on
    // another host) are followed by WinHTTP with full certificate validation
    // per hop; only a downgrade to plain HTTP is refused. Security flags are
    // never relaxed.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!register_request(dl, req)) {
        WinHttpCloseHandle(req);
        req = NULL;
        r = NANUM_DL_CANCEL;
        goto out;
    }

    if (!WinHttpSendRequest(req, L"Accept-Encoding: identity\r\n", (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL))
    {
        r = net_fail(dl, host, "requesting the file");
        goto out;
    }
    DWORD code = 0, len = sizeof(code);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                             WINHTTP_NO_HEADER_INDEX))
    {
        r = net_fail(dl, host, "reading the response");
        goto out;
    }
    if (code != 200) {
        nanum_dl_fail(dl, "The server %s answered HTTP %lu instead of 200 for %s.", host,
                      (unsigned long)code, item->file);
        goto out;
    }
    wchar_t clen[32] = L"";
    len = sizeof(clen);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            clen, &len, WINHTTP_NO_HEADER_INDEX))
    {
        unsigned long long announced = _wcstoui64(clen, NULL, 10);
        if (announced != (unsigned long long)item->size) {
            nanum_dl_fail(dl, "The server %s announced %llu bytes for %s, expected %" PRId64
                          " bytes.", host, announced, item->file, item->size);
            goto out;
        }
    }

    if (!sha_begin(&sha)) {
        nanum_dl_fail(dl, "SHA-256 is not available (Windows CNG failed)");
        goto out;
    }
    phase(dl, NANUM_DL_DOWNLOADING, 0, item->size);
    int64_t received = 0;
    for (;;) {
        if (nanum_dl_cancelled(dl)) {
            r = NANUM_DL_CANCEL;
            goto out;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, dl->buf, NANUM_DL_IO_BUF, &got)) {
            r = net_fail(dl, host, "downloading");
            goto out;
        }
        if (!got)
            break;
        received += got;
        if (received > item->size) {
            nanum_dl_fail(dl, "The server %s sent more than the expected %" PRId64
                          " bytes for %s.", host, item->size, item->file);
            goto out;
        }
        DWORD written = 0;
        if (!sha_update(&sha, dl->buf, got) ||
            !WriteFile(file, dl->buf, got, &written, NULL) || written != got)
        {
            char why[1024];
            nanum_dl_error_text(why, sizeof(why), GetLastError());
            nanum_dl_fail(dl, "Cannot write %s: %s", part, why);
            goto out;
        }
        progress(dl, received);
    }
    if (received != item->size) {
        nanum_dl_fail(dl, "The download of %s ended after %" PRId64 " of %" PRId64
                      " bytes (connection closed early).", item->file, received, item->size);
        goto out;
    }

    phase(dl, NANUM_DL_VERIFYING, received, item->size);
    char hex[65] = "";
    if (!sha_finish(&sha, hex)) {
        nanum_dl_fail(dl, "SHA-256 computation failed (Windows CNG)");
        goto out;
    }
    if (strcmp(hex, item->sha)) {
        nanum_dl_fail(dl, "SHA-256 mismatch for %s from %s: expected %s, got %s. "
                      "The file was discarded and nothing was installed.", item->file, host,
                      item->sha, hex);
        goto out;
    }
    if (!FlushFileBuffers(file)) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        nanum_dl_fail(dl, "Cannot write %s: %s", part, why);
        goto out;
    }
    r = NANUM_DL_OK;

out:
    if (r == NANUM_DL_FAIL && nanum_dl_cancelled(dl))
        r = NANUM_DL_CANCEL;
    sha_free(&sha);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (req) {
        // Whoever swaps dl->req out closes it; if the cancel did, it is gone.
        mp_mutex_lock(&dl->lock);
        bool mine = dl->req == req;
        if (mine)
            dl->req = NULL;
        mp_mutex_unlock(&dl->lock);
        if (mine)
            WinHttpCloseHandle(req);
    }
    if (conn)
        WinHttpCloseHandle(conn);
    if (session)
        WinHttpCloseHandle(session);
    talloc_free(tmp);
    return r;
}

enum nanum_dl_res nanum_dl_get_text(struct nanum_dl *dl, const char *url, int64_t max,
                                    void *ctx, char **out)
{
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r = NANUM_DL_FAIL;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    char host[260] = "";
    char *text = NULL;
    size_t used = 0;

    wchar_t *wurl = W(tmp, url);
    wchar_t whost[256], wpath[2048], wextra[2048];
    URL_COMPONENTS uc = {
        .dwStructSize = sizeof(uc),
        .lpszHostName = whost, .dwHostNameLength = MP_ARRAY_SIZE(whost),
        .lpszUrlPath = wpath, .dwUrlPathLength = MP_ARRAY_SIZE(wpath),
        .lpszExtraInfo = wextra, .dwExtraInfoLength = MP_ARRAY_SIZE(wextra),
    };
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        nanum_dl_fail(dl, "%s is not a valid HTTPS URL", url);
        goto out;
    }
    narrow(whost, uc.dwHostNameLength, host, sizeof(host));
    size_t plen = uc.dwUrlPathLength + uc.dwExtraInfoLength;
    wchar_t *wobj = talloc_array(tmp, wchar_t, plen + 1);
    wmemcpy(wobj, wpath, uc.dwUrlPathLength);
    wmemcpy(wobj + uc.dwUrlPathLength, wextra, uc.dwExtraInfoLength);
    wobj[plen] = 0;
    whost[uc.dwHostNameLength] = 0;

    phase(dl, NANUM_DL_CHECKING, 0, 0);
    session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r = net_fail(dl, host, "starting the HTTP session");
        goto out;
    }
    WinHttpSetTimeouts(session, 30000, 30000, 30000, 60000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    conn = WinHttpConnect(session, whost, uc.nPort, 0);
    if (!conn) {
        r = net_fail(dl, host, "connecting");
        goto out;
    }
    req = WinHttpOpenRequest(conn, L"GET", wobj, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        r = net_fail(dl, host, "preparing the request");
        goto out;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!register_request(dl, req)) {
        WinHttpCloseHandle(req);
        req = NULL;
        r = NANUM_DL_CANCEL;
        goto out;
    }
    if (!WinHttpSendRequest(req, L"Accept-Encoding: identity\r\n", (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL))
    {
        r = net_fail(dl, host, "requesting the release information");
        goto out;
    }
    DWORD code = 0, len = sizeof(code);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                             WINHTTP_NO_HEADER_INDEX))
    {
        r = net_fail(dl, host, "reading the response");
        goto out;
    }
    if (code != 200) {
        nanum_dl_fail(dl, "The server %s answered HTTP %lu instead of 200 for %s.", host,
                      (unsigned long)code, url);
        goto out;
    }
    for (;;) {
        if (nanum_dl_cancelled(dl)) {
            r = NANUM_DL_CANCEL;
            goto out;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, dl->buf, NANUM_DL_IO_BUF, &got)) {
            r = net_fail(dl, host, "downloading");
            goto out;
        }
        if (!got)
            break;
        if ((int64_t)(used + got) > max) {
            nanum_dl_fail(dl, "The answer of %s for %s is larger than %" PRId64 " bytes.", host,
                          url, max);
            goto out;
        }
        text = talloc_realloc(ctx, text, char, used + got + 1);
        memcpy(text + used, dl->buf, got);
        used += got;
    }
    text = talloc_realloc(ctx, text, char, used + 1);
    text[used] = 0;
    *out = text;
    text = NULL;
    r = NANUM_DL_OK;

out:
    if (r == NANUM_DL_FAIL && nanum_dl_cancelled(dl))
        r = NANUM_DL_CANCEL;
    talloc_free(text);
    if (req) {
        mp_mutex_lock(&dl->lock);
        bool mine = dl->req == req;
        if (mine)
            dl->req = NULL;
        mp_mutex_unlock(&dl->lock);
        if (mine)
            WinHttpCloseHandle(req);
    }
    if (conn)
        WinHttpCloseHandle(conn);
    if (session)
        WinHttpCloseHandle(session);
    talloc_free(tmp);
    return r;
}

bool nanum_dl_prepare_dir(struct nanum_dl *dl, const char *final_path, int64_t needed,
                          const char *what)
{
    void *tmp = talloc_new(NULL);
    char *dir = talloc_strdup(tmp, final_path);
    char *slash = strrchr(dir, '\\');
    if (slash)
        *slash = 0;
    wchar_t *wdir = W(tmp, dir);
    int ec = SHCreateDirectoryExW(NULL, wdir, NULL);
    if (ec != ERROR_SUCCESS && ec != ERROR_ALREADY_EXISTS && ec != ERROR_FILE_EXISTS) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), ec);
        nanum_dl_fail(dl, "Cannot create the folder %s: %s", dir, why);
        talloc_free(tmp);
        return false;
    }
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExW(wdir, &avail, NULL, NULL) &&
        avail.QuadPart < (uint64_t)(needed + DISK_HEADROOM))
    {
        nanum_dl_fail(dl, "Not enough free disk space for %s in %s: %" PRId64 " MiB needed, %"
                      PRIu64 " MiB available.", what, dir, (needed + DISK_HEADROOM) >> 20,
                      (uint64_t)(avail.QuadPart >> 20));
        talloc_free(tmp);
        return false;
    }
    talloc_free(tmp);
    return true;
}

enum nanum_dl_res nanum_dl_install(struct nanum_dl *dl, const struct nanum_dl_item *item,
                                   const char *final_path, const char *part,
                                   const struct nanum_dl_fst *seen)
{
    void *tmp = talloc_new(NULL);
    wchar_t *wpart = W(tmp, part), *wfinal = W(tmp, final_path);
    enum nanum_dl_res r = NANUM_DL_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        struct nanum_dl_fst cur;
        if (nanum_dl_stat(final_path, &cur)) {
            bool unchanged = seen && seen->exists && cur.size == seen->size &&
                             cur.mtime == seen->mtime;
            if (!unchanged) {
                bool match = false;
                enum nanum_dl_res h = nanum_dl_hash(dl, item, final_path, &match);
                if (h != NANUM_DL_OK) {
                    r = h;
                    goto out;
                }
                if (match) {
                    DeleteFileW(wpart);
                    r = NANUM_DL_OK;
                    goto out;
                }
            }
            if (MoveFileExW(wpart, wfinal, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                r = NANUM_DL_OK;
                goto out;
            }
        } else {
            if (MoveFileExW(wpart, wfinal, MOVEFILE_WRITE_THROUGH)) {
                r = NANUM_DL_OK;
                goto out;
            }
            DWORD err = GetLastError();
            if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)
                continue;       // somebody else was faster; look at it
        }
        break;
    }
    char why[1024];
    DWORD last = GetLastError();
    nanum_dl_error_text(why, sizeof(why), last);
    nanum_dl_fail(dl, "Cannot install %s: %s%s", final_path, why,
                  last == ERROR_SHARING_VIOLATION || last == ERROR_ACCESS_DENIED
                      ? " The file may be in use by a playing file; close it and retry." : "");
out:
    talloc_free(tmp);
    return r;
}
