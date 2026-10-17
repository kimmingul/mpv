/*
 * NanumPlayer: durable media library (SQLite). See library.h.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation;
 * either version 2 of the License, or (at your option)
 * any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "mpv_talloc.h"
#include "misc/path_utils.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "nanum/i18n.h"
#include "nanum/library.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define SCHEMA_VERSION 2

// ---------------------------------------------------------------------------
// SHA-256

struct sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t fill;
};

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) +
                      K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(struct sha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

static void sha256_update(struct sha256 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    while (len) {
        size_t n = 64 - s->fill;
        if (n > len)
            n = len;
        memcpy(s->buf + s->fill, p, n);
        s->fill += n;
        p += n;
        len -= n;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha256_hex(struct sha256 *s, char out[65])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56)
        sha256_update(s, &pad, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lenbuf, 8);
    for (int i = 0; i < 8; i++)
        snprintf(out + i * 8, 9, "%08x", s->h[i]);
}

void lib_sha256_hex(const void *data, size_t len, char out[65])
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_hex(&s, out);
}

// Hashes len bytes at offset (and the offset itself).
static bool fp_block(FILE *f, struct sha256 *s, int64_t off, int64_t len)
{
    uint8_t buf[16 * 1024];
    if (fseeko(f, off, SEEK_SET))
        return false;
    sha256_update(s, &off, sizeof(off));
    while (len > 0) {
        size_t want = len < (int64_t)sizeof(buf) ? (size_t)len : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (got != want)
            return false;
        sha256_update(s, buf, got);
        len -= got;
    }
    return true;
}

bool lib_fingerprint_file(const char *path, char out_hex[65], int64_t *size,
                          char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, nanum_tr("cannot open the media file"));
        return false;
    }
    bool ok = !fseeko(f, 0, SEEK_END);
    int64_t sz = ok ? (int64_t)ftell(f) : -1;
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, &sz, sizeof(sz));
    if (!ok || sz <= 0) {
        ok = false;
    } else if (sz <= LIB_FP_FULL_MAX) {
        ok = fp_block(f, &s, 0, sz);
    } else {
        ok = fp_block(f, &s, 0, LIB_FP_EDGE) &&
             fp_block(f, &s, sz - LIB_FP_EDGE, LIB_FP_EDGE);
        int64_t span = sz - 2 * (int64_t)LIB_FP_EDGE - LIB_FP_BLOCK;
        for (int i = 1; ok && i <= LIB_FP_SAMPLES; i++)
            ok = fp_block(f, &s, LIB_FP_EDGE + span * i / (LIB_FP_SAMPLES + 1), LIB_FP_BLOCK);
    }
    fclose(f);
    if (!ok) {
        snprintf(err, errsz, nanum_tr("cannot read the media file"));
        return false;
    }
    sha256_hex(&s, out_hex);
    *size = sz;
    return true;
}

// ---------------------------------------------------------------------------
// Database

enum {
    Q_MEDIA_SEL, Q_MEDIA_INS, Q_MEDIA_UPD, Q_SRC_REV, Q_TR_SEL, Q_CUES_SRC, Q_CUES_TR,
    Q_SOURCE_INS, Q_SOURCE_SEL, Q_SOURCE_UPD, Q_REV_INS, Q_CUE_INS, Q_TR_INS,
    Q_TRCUE_INS, Q_TR_TRIM, Q_PRUNE, Q_JOB_INS, Q_JOB_END, Q_PROP_GET, Q_PROP_SET,
    Q_PROP_DEL, Q_LIVE_CUES, Q_LIVE_RANGES, Q_LIVE_DEL_CUES, Q_LIVE_DEL_RANGES,
    Q_LIVE_INS_CUE, Q_LIVE_INS_RANGE, Q_COUNT
};

static const char *const sql_text[Q_COUNT] = {
    [Q_MEDIA_SEL] = "SELECT id FROM media WHERE key=?1",
    [Q_MEDIA_INS] = "INSERT INTO media(key,kind,name,size,duration,created,seen) "
                    "VALUES(?1,?2,?3,?4,?5,?6,?6)",
    [Q_MEDIA_UPD] = "UPDATE media SET seen=?2, name=?3, duration=?4 WHERE id=?1",
    [Q_SRC_REV] = "SELECT s.id, r.id, r.origin, r.language, r.engine, r.lines, r.created "
                  "FROM source s JOIN source_rev r ON r.source_id=s.id "
                  "WHERE s.media_id=?1 AND s.key=?2 ORDER BY r.rev DESC LIMIT 1",
    [Q_TR_SEL] = "SELECT t.id, t.rev_id, t.model, t.engine, t.target, t.failed, t.created "
                 "FROM translation t JOIN source_rev r ON t.rev_id=r.id "
                 "WHERE r.source_id=?1 AND t.target=?2 ORDER BY t.created DESC, t.id DESC LIMIT 1",
    [Q_CUES_SRC] = "SELECT pts, dur, text, NULL FROM source_cue WHERE rev_id=?1 ORDER BY idx",
    [Q_CUES_TR] = "SELECT c.pts, c.dur, c.text, IFNULL(t.text, '') FROM source_cue c "
                  "LEFT JOIN translation_cue t ON t.tr_id=?2 AND t.idx=c.idx "
                  "WHERE c.rev_id=?1 ORDER BY c.idx",
    [Q_SOURCE_INS] = "INSERT OR IGNORE INTO source(media_id,key,input,track_id,language,engine,created) "
                     "VALUES(?1,?2,?3,?4,?5,?6,?7)",
    [Q_SOURCE_SEL] = "SELECT id FROM source WHERE media_id=?1 AND key=?2",
    [Q_SOURCE_UPD] = "UPDATE source SET language=?2, engine=?3 WHERE id=?1",
    [Q_REV_INS] = "INSERT INTO source_rev(source_id,rev,origin,language,engine,lines,created) "
                  "VALUES(?1,(SELECT IFNULL(MAX(rev),0)+1 FROM source_rev WHERE source_id=?1),"
                  "?2,?3,?4,?5,?6)",
    [Q_CUE_INS] = "INSERT INTO source_cue(rev_id,idx,pts,dur,text) VALUES(?1,?2,?3,?4,?5)",
    [Q_TR_INS] = "INSERT INTO translation(rev_id,rev,target,engine,model,lines,failed,created) "
                 "VALUES(?1,(SELECT IFNULL(MAX(t.rev),0)+1 FROM translation t "
                 "JOIN source_rev r ON t.rev_id=r.id WHERE r.source_id="
                 "(SELECT source_id FROM source_rev WHERE id=?1)),?2,?3,?4,?5,?6,?7)",
    [Q_TRCUE_INS] = "INSERT INTO translation_cue(tr_id,idx,text) VALUES(?1,?2,?3)",
    [Q_TR_TRIM] = "DELETE FROM translation WHERE rev_id=?1 AND id NOT IN "
                  "(SELECT id FROM translation WHERE rev_id=?1 ORDER BY id DESC LIMIT 5)",
    // Older revisions go only if every target they hold is covered by the
    // newest revision.
    [Q_PRUNE] = "DELETE FROM source_rev WHERE source_id=(SELECT source_id FROM source_rev "
                "WHERE id=?1) AND id<>?1 AND rev<(SELECT rev FROM source_rev WHERE id=?1) "
                "AND NOT EXISTS (SELECT 1 FROM translation t WHERE t.rev_id=source_rev.id "
                "AND t.target NOT IN (SELECT target FROM translation WHERE rev_id=?1))",
    [Q_JOB_INS] = "INSERT INTO job(source_id,kind,state,pid,started) VALUES(?1,?2,'running',?3,?4)",
    [Q_JOB_END] = "UPDATE job SET state=?2, error=?3, finished=?4 WHERE id=?1",
    [Q_PROP_GET] = "SELECT value FROM media_prop WHERE media_id=?1 AND key=?2",
    [Q_PROP_SET] = "INSERT OR REPLACE INTO media_prop(media_id,key,value) VALUES(?1,?2,?3)",
    [Q_PROP_DEL] = "DELETE FROM media_prop WHERE media_id=?1 AND key=?2",
    [Q_LIVE_CUES] = "SELECT pts, dur, text, tr FROM live_cue WHERE media_id=?1 AND key=?2 "
                    "ORDER BY idx",
    [Q_LIVE_RANGES] = "SELECT a, b FROM live_range WHERE media_id=?1 AND key=?2 ORDER BY idx",
    [Q_LIVE_DEL_CUES] = "DELETE FROM live_cue WHERE media_id=?1 AND key=?2",
    [Q_LIVE_DEL_RANGES] = "DELETE FROM live_range WHERE media_id=?1 AND key=?2",
    [Q_LIVE_INS_CUE] = "INSERT INTO live_cue(media_id,key,idx,pts,dur,text,tr) "
                       "VALUES(?1,?2,?3,?4,?5,?6,?7)",
    [Q_LIVE_INS_RANGE] = "INSERT INTO live_range(media_id,key,idx,a,b) VALUES(?1,?2,?3,?4,?5)",
};

struct nanum_lib {
    sqlite3 *db;
    mp_mutex lock;
    sqlite3_stmt *stmt[Q_COUNT];
    char err[256];
};

const char *lib_error(struct nanum_lib *l)
{
    return l->err;
}

static void set_err(struct nanum_lib *l, const char *what)
{
    snprintf(l->err, sizeof(l->err), "%s: %s", nanum_tr(what), l->db ? sqlite3_errmsg(l->db) : "?");
}

static sqlite3_stmt *Q(struct nanum_lib *l, int id)
{
    if (!l->stmt[id] &&
        sqlite3_prepare_v2(l->db, sql_text[id], -1, &l->stmt[id], NULL) != SQLITE_OK) {
        set_err(l, N_("prepare"));
        l->stmt[id] = NULL;
        return NULL;
    }
    sqlite3_reset(l->stmt[id]);
    sqlite3_clear_bindings(l->stmt[id]);
    return l->stmt[id];
}

static bool exec(struct nanum_lib *l, const char *sql)
{
    char *msg = NULL;
    if (sqlite3_exec(l->db, sql, NULL, NULL, &msg) != SQLITE_OK) {
        snprintf(l->err, sizeof(l->err), "%.100s: %s", sql, msg ? msg : "?");
        sqlite3_free(msg);
        return false;
    }
    return true;
}

static void bind_text(sqlite3_stmt *st, int i, const char *s)
{
    sqlite3_bind_text(st, i, s ? s : "", -1, SQLITE_TRANSIENT);
}

static const char schema[] =
    "CREATE TABLE IF NOT EXISTS media(id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE,"
    " kind TEXT, name TEXT, size INTEGER, duration REAL, created INTEGER, seen INTEGER);"
    "CREATE TABLE IF NOT EXISTS source(id INTEGER PRIMARY KEY,"
    " media_id INTEGER NOT NULL REFERENCES media(id) ON DELETE CASCADE,"
    " key TEXT NOT NULL, input TEXT, track_id INTEGER, language TEXT, engine TEXT,"
    " created INTEGER, UNIQUE(media_id, key));"
    "CREATE TABLE IF NOT EXISTS source_rev(id INTEGER PRIMARY KEY,"
    " source_id INTEGER NOT NULL REFERENCES source(id) ON DELETE CASCADE,"
    " rev INTEGER NOT NULL, origin REAL, language TEXT, engine TEXT, lines INTEGER,"
    " created INTEGER, UNIQUE(source_id, rev));"
    "CREATE TABLE IF NOT EXISTS source_cue(rev_id INTEGER NOT NULL"
    " REFERENCES source_rev(id) ON DELETE CASCADE, idx INTEGER NOT NULL, pts REAL, dur REAL,"
    " text TEXT NOT NULL, PRIMARY KEY(rev_id, idx)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS translation(id INTEGER PRIMARY KEY,"
    " rev_id INTEGER NOT NULL REFERENCES source_rev(id) ON DELETE CASCADE,"
    " rev INTEGER NOT NULL, target TEXT, engine TEXT, model TEXT, lines INTEGER,"
    " failed INTEGER, created INTEGER);"
    "CREATE TABLE IF NOT EXISTS translation_cue(tr_id INTEGER NOT NULL"
    " REFERENCES translation(id) ON DELETE CASCADE, idx INTEGER NOT NULL,"
    " text TEXT NOT NULL, PRIMARY KEY(tr_id, idx)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS job(id INTEGER PRIMARY KEY, source_id INTEGER, kind TEXT,"
    " state TEXT, error TEXT, pid INTEGER, started INTEGER, finished INTEGER);"
    "CREATE INDEX IF NOT EXISTS translation_by_rev ON translation(rev_id);"
    // Schema v2
    "CREATE TABLE IF NOT EXISTS media_prop(media_id INTEGER NOT NULL"
    " REFERENCES media(id) ON DELETE CASCADE, key TEXT NOT NULL, value TEXT NOT NULL,"
    " PRIMARY KEY(media_id, key)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS live_cue(media_id INTEGER NOT NULL"
    " REFERENCES media(id) ON DELETE CASCADE, key TEXT NOT NULL, idx INTEGER NOT NULL,"
    " pts REAL, dur REAL, text TEXT NOT NULL, tr TEXT,"
    " PRIMARY KEY(media_id, key, idx)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS live_range(media_id INTEGER NOT NULL"
    " REFERENCES media(id) ON DELETE CASCADE, key TEXT NOT NULL, idx INTEGER NOT NULL,"
    " a REAL, b REAL, PRIMARY KEY(media_id, key, idx)) WITHOUT ROWID;";

static bool pid_alive(int64_t pid)
{
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return true;
#endif
}

// A crashed process leaves 'running' job rows behind.
static void reap_jobs(struct nanum_lib *l)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(l->db, "SELECT id, pid FROM job WHERE state='running'", -1, &st,
                           NULL) != SQLITE_OK)
        return;
    int64_t ids[256];
    int n = 0;
    while (n < 256 && sqlite3_step(st) == SQLITE_ROW) {
        if (!pid_alive(sqlite3_column_int64(st, 1)))
            ids[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    for (int i = 0; i < n; i++) {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "UPDATE job SET state='interrupted', finished=%lld WHERE id=%lld",
                 (long long)time(NULL), (long long)ids[i]);
        exec(l, sql);
    }
}

static void lib_destroy(void *p)
{
    struct nanum_lib *l = p;
    for (int i = 0; i < Q_COUNT; i++)
        sqlite3_finalize(l->stmt[i]);
    if (l->db) {
        sqlite3_close(l->db);
        l->db = NULL;
    }
    mp_mutex_destroy(&l->lock);
}

struct nanum_lib *lib_open(const char *path, char *err, size_t errsz)
{
    void *tmp = talloc_new(NULL);
    if (!path || !path[0]) {
        snprintf(err, errsz, nanum_tr("no library file given"));
        talloc_free(tmp);
        return NULL;
    }
    char *file = talloc_strdup(tmp, path);
    char *dir = talloc_strdup(tmp, file);
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
    if (slash && slash != dir) {
        *slash = '\0';
        mp_mkdirp(dir);
    }

    struct nanum_lib *l = talloc_zero(NULL, struct nanum_lib);
    mp_mutex_init(&l->lock);
    talloc_set_destructor(l, lib_destroy);
    int rc = sqlite3_open_v2(file, &l->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        snprintf(err, errsz, nanum_tr("cannot open the library '%s': %s"), file,
                 l->db ? sqlite3_errmsg(l->db) : "out of memory");
        goto fail;
    }
    sqlite3_busy_timeout(l->db, 5000);
    if (!exec(l, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"))
        goto fail_err;

    sqlite3_stmt *st = NULL;
    int version = 0;
    if (sqlite3_prepare_v2(l->db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        version = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (version > SCHEMA_VERSION) {
        snprintf(err, errsz, nanum_tr("the library '%s' was written by a newer version"), file);
        goto fail;
    }
    if (version < SCHEMA_VERSION) {
        char ver[64];
        snprintf(ver, sizeof(ver), "PRAGMA user_version=%d;", SCHEMA_VERSION);
        if (!exec(l, "BEGIN IMMEDIATE") || !exec(l, schema) || !exec(l, ver) ||
            !exec(l, "COMMIT")) {
            exec(l, "ROLLBACK");
            goto fail_err;
        }
    }
    reap_jobs(l);
    talloc_free(tmp);
    return l;

fail_err:
    snprintf(err, errsz, "%s", l->err);
fail:
    talloc_free(l);
    talloc_free(tmp);
    return NULL;
}

void lib_close(struct nanum_lib *l)
{
    talloc_free(l);
}

int64_t lib_media_ensure(struct nanum_lib *l, const char *key, const char *kind,
                         const char *name, int64_t size, double duration)
{
    int64_t id = 0;
    mp_mutex_lock(&l->lock);
    if (!exec(l, "BEGIN IMMEDIATE"))
        goto out;
    sqlite3_stmt *st = Q(l, Q_MEDIA_SEL);
    if (!st)
        goto rollback;
    bind_text(st, 1, key);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
        sqlite3_reset(st);
        if (!(st = Q(l, Q_MEDIA_UPD)))
            goto rollback;
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, time(NULL));
        bind_text(st, 3, name);
        sqlite3_bind_double(st, 4, duration);
        if (sqlite3_step(st) != SQLITE_DONE) {
            set_err(l, N_("media update"));
            id = 0;
            goto rollback;
        }
    } else {
        sqlite3_reset(st);
        if (!(st = Q(l, Q_MEDIA_INS)))
            goto rollback;
        bind_text(st, 1, key);
        bind_text(st, 2, kind);
        bind_text(st, 3, name);
        sqlite3_bind_int64(st, 4, size);
        sqlite3_bind_double(st, 5, duration);
        sqlite3_bind_int64(st, 6, time(NULL));
        if (sqlite3_step(st) != SQLITE_DONE) {
            set_err(l, N_("media insert"));
            goto rollback;
        }
        id = sqlite3_last_insert_rowid(l->db);
    }
    if (!exec(l, "COMMIT"))
        id = 0;
    else
        goto out;
rollback:
    exec(l, "ROLLBACK");
    id = 0;
out:
    mp_mutex_unlock(&l->lock);
    return id;
}

bool lib_lookup(struct nanum_lib *l, int64_t media_id, const char *source_key,
                const char *target, struct lib_found *out)
{
    *out = (struct lib_found){0};
    bool ok = false;
    mp_mutex_lock(&l->lock);
    sqlite3_stmt *st = Q(l, Q_SRC_REV);
    if (!st)
        goto out;
    sqlite3_bind_int64(st, 1, media_id);
    bind_text(st, 2, source_key);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->has_source = true;
        out->source_id = sqlite3_column_int64(st, 0);
        out->source_rev = sqlite3_column_int64(st, 1);
        out->origin = sqlite3_column_double(st, 2);
        snprintf(out->src_lang, sizeof(out->src_lang), "%s",
                 (const char *)sqlite3_column_text(st, 3));
        snprintf(out->src_engine, sizeof(out->src_engine), "%s",
                 (const char *)sqlite3_column_text(st, 4));
        out->src_lines = sqlite3_column_int(st, 5);
        out->src_created = sqlite3_column_int64(st, 6);
    } else if (rc != SQLITE_DONE) {
        set_err(l, N_("lookup"));
        goto out;
    }
    sqlite3_reset(st);
    if (out->has_source) {
        if (!(st = Q(l, Q_TR_SEL)))
            goto out;
        sqlite3_bind_int64(st, 1, out->source_id);
        bind_text(st, 2, target);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            out->has_tr = true;
            out->tr_id = sqlite3_column_int64(st, 0);
            out->tr_source_rev = sqlite3_column_int64(st, 1);
            snprintf(out->tr_model, sizeof(out->tr_model), "%s",
                     (const char *)sqlite3_column_text(st, 2));
            snprintf(out->tr_engine, sizeof(out->tr_engine), "%s",
                     (const char *)sqlite3_column_text(st, 3));
            snprintf(out->tr_target, sizeof(out->tr_target), "%s",
                     (const char *)sqlite3_column_text(st, 4));
            out->tr_failed = sqlite3_column_int(st, 5);
            out->tr_created = sqlite3_column_int64(st, 6);
        } else if (rc != SQLITE_DONE) {
            set_err(l, N_("lookup"));
            goto out;
        }
        sqlite3_reset(st);
    }
    ok = true;
out:
    mp_mutex_unlock(&l->lock);
    return ok;
}

bool lib_load_cues(struct nanum_lib *l, void *ctx, int64_t source_rev, int64_t tr_id,
                   struct lib_cue **cues, size_t *n, double *origin)
{
    *cues = NULL;
    *n = 0;
    bool ok = false;
    mp_mutex_lock(&l->lock);
    sqlite3_stmt *st = tr_id ? Q(l, Q_CUES_TR) : Q(l, Q_CUES_SRC);
    if (!st)
        goto out;
    sqlite3_bind_int64(st, 1, source_rev);
    if (tr_id)
        sqlite3_bind_int64(st, 2, tr_id);
    size_t cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (*n == cap) {
            cap = cap ? cap * 2 : 256;
            *cues = talloc_realloc(ctx, *cues, struct lib_cue, cap);
        }
        struct lib_cue *c = &(*cues)[(*n)++];
        c->pts = sqlite3_column_double(st, 0);
        c->dur = sqlite3_column_double(st, 1);
        c->src = talloc_strdup(*cues, (const char *)sqlite3_column_text(st, 2));
        c->tr = sqlite3_column_type(st, 3) == SQLITE_NULL
                ? NULL : talloc_strdup(*cues, (const char *)sqlite3_column_text(st, 3));
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) {
        set_err(l, N_("load cues"));
        talloc_free(*cues);
        *cues = NULL;
        *n = 0;
        goto out;
    }
    if (origin) {
        *origin = 0;
        sqlite3_stmt *o = NULL;
        if (sqlite3_prepare_v2(l->db, "SELECT origin FROM source_rev WHERE id=?1", -1, &o,
                               NULL) == SQLITE_OK) {
            sqlite3_bind_int64(o, 1, source_rev);
            if (sqlite3_step(o) == SQLITE_ROW)
                *origin = sqlite3_column_double(o, 0);
        }
        sqlite3_finalize(o);
    }
    ok = *n > 0;
    if (!ok)
        snprintf(l->err, sizeof(l->err), "%s", nanum_tr("the saved transcript is empty"));
out:
    mp_mutex_unlock(&l->lock);
    return ok;
}

int64_t lib_save_source(struct nanum_lib *l, int64_t media_id, const char *source_key,
                        const char *input, int track_id, const char *language,
                        const char *engine, double origin,
                        const struct lib_cue *cues, size_t n)
{
    int64_t rev = 0;
    mp_mutex_lock(&l->lock);
    if (!exec(l, "BEGIN IMMEDIATE"))
        goto out;
    sqlite3_stmt *st = Q(l, Q_SOURCE_INS);
    if (!st)
        goto rollback;
    sqlite3_bind_int64(st, 1, media_id);
    bind_text(st, 2, source_key);
    bind_text(st, 3, input);
    sqlite3_bind_int(st, 4, track_id);
    bind_text(st, 5, language);
    bind_text(st, 6, engine);
    sqlite3_bind_int64(st, 7, time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) {
        set_err(l, N_("source insert"));
        goto rollback;
    }
    if (!(st = Q(l, Q_SOURCE_SEL)))
        goto rollback;
    sqlite3_bind_int64(st, 1, media_id);
    bind_text(st, 2, source_key);
    if (sqlite3_step(st) != SQLITE_ROW) {
        set_err(l, N_("source select"));
        goto rollback;
    }
    int64_t source_id = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);

    if (!(st = Q(l, Q_SOURCE_UPD)))
        goto rollback;
    sqlite3_bind_int64(st, 1, source_id);
    bind_text(st, 2, language);
    bind_text(st, 3, engine);
    if (sqlite3_step(st) != SQLITE_DONE) {
        set_err(l, N_("source update"));
        goto rollback;
    }

    if (!(st = Q(l, Q_REV_INS)))
        goto rollback;
    sqlite3_bind_int64(st, 1, source_id);
    sqlite3_bind_double(st, 2, origin);
    bind_text(st, 3, language);
    bind_text(st, 4, engine);
    sqlite3_bind_int64(st, 5, (int64_t)n);
    sqlite3_bind_int64(st, 6, time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) {
        set_err(l, N_("revision insert"));
        goto rollback;
    }
    int64_t rev_id = sqlite3_last_insert_rowid(l->db);
    for (size_t i = 0; i < n; i++) {
        if (!(st = Q(l, Q_CUE_INS)))
            goto rollback;
        sqlite3_bind_int64(st, 1, rev_id);
        sqlite3_bind_int64(st, 2, (int64_t)i);
        sqlite3_bind_double(st, 3, cues[i].pts);
        sqlite3_bind_double(st, 4, cues[i].dur);
        bind_text(st, 5, cues[i].src);
        if (sqlite3_step(st) != SQLITE_DONE) {
            set_err(l, N_("cue insert"));
            goto rollback;
        }
    }
    if (!exec(l, "COMMIT"))
        goto rollback;
    rev = rev_id;
    goto out;
rollback:
    exec(l, "ROLLBACK");
out:
    mp_mutex_unlock(&l->lock);
    return rev;
}

int64_t lib_save_translation(struct nanum_lib *l, int64_t source_rev, const char *target,
                             const char *engine, const char *model,
                             const struct lib_cue *cues, size_t n)
{
    int64_t tr = 0;
    int failed = 0;
    for (size_t i = 0; i < n; i++)
        failed += !cues[i].tr || !cues[i].tr[0];
    mp_mutex_lock(&l->lock);
    if (!exec(l, "BEGIN IMMEDIATE"))
        goto out;
    sqlite3_stmt *st = Q(l, Q_TR_INS);
    if (!st)
        goto rollback;
    sqlite3_bind_int64(st, 1, source_rev);
    bind_text(st, 2, target);
    bind_text(st, 3, engine);
    bind_text(st, 4, model);
    sqlite3_bind_int64(st, 5, (int64_t)n);
    sqlite3_bind_int(st, 6, failed);
    sqlite3_bind_int64(st, 7, time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) {
        set_err(l, N_("translation insert"));
        goto rollback;
    }
    int64_t id = sqlite3_last_insert_rowid(l->db);
    for (size_t i = 0; i < n; i++) {
        if (!(st = Q(l, Q_TRCUE_INS)))
            goto rollback;
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, (int64_t)i);
        bind_text(st, 3, cues[i].tr);
        if (sqlite3_step(st) != SQLITE_DONE) {
            set_err(l, N_("translation cue insert"));
            goto rollback;
        }
    }
    if (!(st = Q(l, Q_TR_TRIM)))
        goto rollback;
    sqlite3_bind_int64(st, 1, source_rev);
    if (sqlite3_step(st) != SQLITE_DONE) {
        set_err(l, N_("translation trim"));
        goto rollback;
    }
    if (!exec(l, "COMMIT"))
        goto rollback;
    tr = id;

    // The new translation is durable: older source revisions it supersedes may
    // go. A failure here only leaves them in place.
    if ((st = Q(l, Q_PRUNE))) {
        sqlite3_bind_int64(st, 1, source_rev);
        if (sqlite3_step(st) != SQLITE_DONE)
            set_err(l, N_("prune"));
    }
    goto out;
rollback:
    exec(l, "ROLLBACK");
out:
    mp_mutex_unlock(&l->lock);
    return tr;
}

int64_t lib_job_begin(struct nanum_lib *l, int64_t source_id, const char *kind)
{
    int64_t id = 0;
    mp_mutex_lock(&l->lock);
    sqlite3_stmt *st = Q(l, Q_JOB_INS);
    if (st) {
        sqlite3_bind_int64(st, 1, source_id);
        bind_text(st, 2, kind);
#ifdef _WIN32
        sqlite3_bind_int64(st, 3, (int64_t)GetCurrentProcessId());
#else
        sqlite3_bind_int64(st, 3, 0);
#endif
        sqlite3_bind_int64(st, 4, time(NULL));
        if (sqlite3_step(st) == SQLITE_DONE)
            id = sqlite3_last_insert_rowid(l->db);
        else
            set_err(l, N_("job insert"));
    }
    mp_mutex_unlock(&l->lock);
    return id;
}

void lib_job_end(struct nanum_lib *l, int64_t job, const char *state, const char *error)
{
    if (!job)
        return;
    mp_mutex_lock(&l->lock);
    sqlite3_stmt *st = Q(l, Q_JOB_END);
    if (st) {
        sqlite3_bind_int64(st, 1, job);
        bind_text(st, 2, state);
        bind_text(st, 3, error);
        sqlite3_bind_int64(st, 4, time(NULL));
        if (sqlite3_step(st) != SQLITE_DONE)
            set_err(l, N_("job update"));
    }
    mp_mutex_unlock(&l->lock);
}

char *lib_prop_get(struct nanum_lib *l, void *ctx, int64_t media_id, const char *key)
{
    char *v = NULL;
    mp_mutex_lock(&l->lock);
    sqlite3_stmt *st = Q(l, Q_PROP_GET);
    if (st) {
        sqlite3_bind_int64(st, 1, media_id);
        bind_text(st, 2, key);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW)
            v = talloc_strdup(ctx, (const char *)sqlite3_column_text(st, 0));
        else if (rc != SQLITE_DONE)
            set_err(l, N_("property read"));
        sqlite3_reset(st);
    }
    mp_mutex_unlock(&l->lock);
    return v;
}

bool lib_prop_set(struct nanum_lib *l, int64_t media_id, const char *key, const char *value)
{
    bool ok = false;
    mp_mutex_lock(&l->lock);
    sqlite3_stmt *st = Q(l, value ? Q_PROP_SET : Q_PROP_DEL);
    if (st) {
        sqlite3_bind_int64(st, 1, media_id);
        bind_text(st, 2, key);
        if (value)
            bind_text(st, 3, value);
        ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok)
            set_err(l, N_("property write"));
        sqlite3_reset(st);
    }
    mp_mutex_unlock(&l->lock);
    return ok;
}

// Runs a prepared statement that takes (media_id, key) to completion.
static bool live_delete(struct nanum_lib *l, int q, int64_t media_id, const char *key)
{
    sqlite3_stmt *st = Q(l, q);
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, media_id);
    bind_text(st, 2, key);
    if (sqlite3_step(st) != SQLITE_DONE) {
        set_err(l, N_("live delete"));
        return false;
    }
    return true;
}

bool lib_live_save(struct nanum_lib *l, int64_t media_id, const char *key,
                   const struct lib_cue *cues, size_t n,
                   const struct lib_range *ranges, size_t nr)
{
    bool ok = false;
    mp_mutex_lock(&l->lock);
    if (!exec(l, "BEGIN IMMEDIATE"))
        goto out;
    if (!live_delete(l, Q_LIVE_DEL_CUES, media_id, key) ||
        !live_delete(l, Q_LIVE_DEL_RANGES, media_id, key))
        goto rollback;
    for (size_t i = 0; i < n; i++) {
        sqlite3_stmt *st = Q(l, Q_LIVE_INS_CUE);
        if (!st)
            goto rollback;
        sqlite3_bind_int64(st, 1, media_id);
        bind_text(st, 2, key);
        sqlite3_bind_int64(st, 3, (int64_t)i);
        sqlite3_bind_double(st, 4, cues[i].pts);
        sqlite3_bind_double(st, 5, cues[i].dur);
        bind_text(st, 6, cues[i].src);
        if (cues[i].tr)
            bind_text(st, 7, cues[i].tr);
        else
            sqlite3_bind_null(st, 7);
        if (sqlite3_step(st) != SQLITE_DONE) {
            set_err(l, N_("live cue insert"));
            goto rollback;
        }
    }
    for (size_t i = 0; i < nr; i++) {
        sqlite3_stmt *st = Q(l, Q_LIVE_INS_RANGE);
        if (!st)
            goto rollback;
        sqlite3_bind_int64(st, 1, media_id);
        bind_text(st, 2, key);
        sqlite3_bind_int64(st, 3, (int64_t)i);
        sqlite3_bind_double(st, 4, ranges[i].a);
        sqlite3_bind_double(st, 5, ranges[i].b);
        if (sqlite3_step(st) != SQLITE_DONE) {
            set_err(l, N_("live range insert"));
            goto rollback;
        }
    }
    if (!exec(l, "COMMIT"))
        goto rollback;
    ok = true;
    goto out;
rollback:
    exec(l, "ROLLBACK");
out:
    mp_mutex_unlock(&l->lock);
    return ok;
}

bool lib_live_load(struct nanum_lib *l, void *ctx, int64_t media_id, const char *key,
                   struct lib_cue **cues, size_t *n, struct lib_range **ranges,
                   size_t *nr)
{
    *cues = NULL;
    *ranges = NULL;
    *n = *nr = 0;
    bool ok = false;
    size_t cap = 0;
    int rc;
    mp_mutex_lock(&l->lock);
    sqlite3_stmt *st = Q(l, Q_LIVE_CUES);
    if (!st)
        goto out;
    sqlite3_bind_int64(st, 1, media_id);
    bind_text(st, 2, key);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (*n == cap) {
            cap = cap ? cap * 2 : 256;
            *cues = talloc_realloc(ctx, *cues, struct lib_cue, cap);
        }
        struct lib_cue *c = &(*cues)[(*n)++];
        c->pts = sqlite3_column_double(st, 0);
        c->dur = sqlite3_column_double(st, 1);
        c->src = talloc_strdup(*cues, (const char *)sqlite3_column_text(st, 2));
        c->tr = sqlite3_column_type(st, 3) == SQLITE_NULL
                ? NULL : talloc_strdup(*cues, (const char *)sqlite3_column_text(st, 3));
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) {
        set_err(l, N_("load live cues"));
        goto fail;
    }
    if (!(st = Q(l, Q_LIVE_RANGES)))
        goto fail;
    sqlite3_bind_int64(st, 1, media_id);
    bind_text(st, 2, key);
    cap = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (*nr == cap) {
            cap = cap ? cap * 2 : 16;
            *ranges = talloc_realloc(ctx, *ranges, struct lib_range, cap);
        }
        (*ranges)[(*nr)++] = (struct lib_range){sqlite3_column_double(st, 0),
                                                sqlite3_column_double(st, 1)};
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) {
        set_err(l, N_("load live ranges"));
        goto fail;
    }
    ok = true;
    goto out;
fail:
    talloc_free(*cues);
    talloc_free(*ranges);
    *cues = NULL;
    *ranges = NULL;
    *n = *nr = 0;
out:
    mp_mutex_unlock(&l->lock);
    return ok;
}
