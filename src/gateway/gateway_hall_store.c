// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_hall_store.c
 * @brief Gateway-side hall event recording (write side).
 *
 * Records gateway-orchestrated sessions (SSE streaming chat, agent.run)
 * into the single-source-of-truth hall event store, aligned byte-for-byte
 * with the runtime writer (atoms/coreloopthree/src/dispatch/hall_store.c):
 * same root (airy_data_dir()/agentrt/hall), same file naming
 * ({tenant}.{task}.{category}.{ts_utc}.{seq:04u}.json), same event body
 * header/access layout. The gateway is a writer process on its own: gseq
 * is per-process (audit only), cross-process order is (ts_utc, seq).
 *
 * prev_file linkage mirrors hall_store.c: each event records the file id of
 * the previous event in the same (task, category) directory (the max-seq
 * file on disk; the runtime writer uses its in-memory index, the gateway
 * scans the directory so the link survives restarts and concurrent
 * writers). This keeps the decision chain reconstructible from the disk
 * event flow alone.
 */

// @owner: team-B
#include "gateway_hall_store.h"

#include "airy_memory.h"
#include "airy_dirent.h"
#include "atomic_compat.h"
#include "platform.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* stat/S_ISDIR for the hall watch read-side directory walk */
#include <sys/stat.h>

#define GW_HALL_TENANT "default"
#define GW_HALL_SCHEMA "task-file-v1"
#define GW_HALL_ROOT_REL "agentrt/hall"
#define GW_HALL_PATH_MAX 1024
#define GW_HALL_TS_LEN 24
#define GW_HALL_FILE_ID_MAX 192

#if defined(_WIN32)
#include <direct.h>
#define GW_HALL_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define GW_HALL_MKDIR(p) mkdir((p), 0755)
#endif

/* Lazy one-time init (thread-safe): 0=uninit, 2=initializing, 1=ready.
 * MHD runs a thread pool and agent.run is synchronous; the event writer
 * must be safe from concurrent SSE connections. */
static atomic_int g_hall_ready = 0;
static airy_mtx_t g_hall_lock;
static atomic_uint_fast64_t g_hall_gseq;

static void gw_hall_ensure_init(void)
{
    while (atomic_load_explicit(&g_hall_ready, memory_order_acquire) != 1) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_hall_ready, &expected, 2,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            airy_mtx_init(&g_hall_lock);
            atomic_store_explicit(&g_hall_gseq, 0, memory_order_relaxed);
            atomic_store_explicit(&g_hall_ready, 1, memory_order_release);
            break;
        }
    }
}

static void gw_hall_ts_utc(char *buf, size_t sz)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &ts.tv_sec);
#else
    gmtime_r(&ts.tv_sec, &tmv);
#endif
    long ms = ts.tv_nsec / 1000000;
    snprintf(buf, sz, "%04d%02d%02dT%02d%02d%02d%03ld", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

static void gw_hall_mkdirs(const char *path)
{
    char tmp[GW_HALL_PATH_MAX];
    AIRY_STRNCPY_TERM(tmp, path, sizeof(tmp));
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            GW_HALL_MKDIR(tmp);
            tmp[i] = saved;
        }
    }
    GW_HALL_MKDIR(tmp);
}

/* Scan one (task, category) dir for existing events. Returns the next seq
 * (max existing seq + 1) so concurrent writer processes (C CLI / gateway)
 * never overwrite each other's event files. When out_prev_file is given it
 * receives the file name of the current max-seq event, i.e. the decision-
 * chain predecessor for this (task, category). */
static unsigned gw_hall_dir_scan(const char *dir, char *out_prev_file, size_t prev_sz)
{
    unsigned max_seq = 0;
    if (out_prev_file && prev_sz > 0)
        out_prev_file[0] = '\0';
    DIR *d = opendir(dir);
    if (!d)
        return 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        size_t len = strlen(n);
        if (len < 5 || strcmp(n + len - 5, ".json") != 0)
            continue;
        /* seq 是倒数第二段（tenant.task.cat.ts.seq.json），与 hall_store.c
         * hall_parse_seq 同规则：取最后一个 '.' 前的段会拿到 "json"，
         * 导致 seq 恒为 1、prev_file 恒空、并发写撞号覆盖。 */
        const char *last_dot = strrchr(n, '.');
        if (!last_dot || last_dot == n)
            continue;
        const char *dot2 = last_dot - 1;
        while (dot2 > n && *dot2 != '.')
            dot2--;
        if (*dot2 != '.')
            continue;
        unsigned seq = 0;
        int digits = 0;
        for (const char *q = dot2 + 1; q < last_dot; q++) {
            if (*q < '0' || *q > '9') {
                seq = 0;
                digits = 0;
                break;
            }
            seq = seq * 10 + (unsigned)(*q - '0');
            digits++;
        }
        if (digits > 0 && seq > max_seq) {
            max_seq = seq;
            if (out_prev_file && prev_sz > 0)
                AIRY_STRNCPY_TERM(out_prev_file, n, prev_sz);
        }
    }
    closedir(d);
    return max_seq + 1;
}

int gw_hall_store_event(const char *task_id, const char *category, const char *node_id,
                        const char *content_json)
{
    if (!task_id || !task_id[0] || !category || !category[0] || !content_json || !content_json[0])
        return -1;

    gw_hall_ensure_init();
    airy_mtx_lock(&g_hall_lock);

    uint64_t gseq = atomic_fetch_add_explicit(&g_hall_gseq, 1, memory_order_relaxed) + 1;

    char ts[GW_HALL_TS_LEN];
    gw_hall_ts_utc(ts, sizeof(ts));

    char dir[GW_HALL_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s/%s", airy_data_dir(), GW_HALL_ROOT_REL,
             GW_HALL_TENANT, task_id, category);
    gw_hall_mkdirs(dir);

    /* Decision-chain predecessor: the max-seq file in this (task, category)
     * dir ("" for the first event), mirroring hall_store.c prev_id. */
    char prev_file[GW_HALL_FILE_ID_MAX] = {0};
    unsigned seq = gw_hall_dir_scan(dir, prev_file, sizeof(prev_file));

    char file_id[GW_HALL_FILE_ID_MAX];
    snprintf(file_id, sizeof(file_id), "%s.%s.%s.%s.%04u.json", GW_HALL_TENANT, task_id, category,
             ts, seq);

    /* Same write-role policy as hall_store.c: cognition-only files vs.
     * execution-class files writable by executors too. */
    const char *write_roles = (strcmp(category, "blueprint") == 0 ||
                               strcmp(category, "command") == 0 ||
                               strcmp(category, "chain") == 0) ?
                                  "[\"cognition\"]" :
                                  "[\"cognition\",\"executor\"]";

    char header[768];
    snprintf(header, sizeof(header),
             "{\"file\":{\"id\":\"%s\",\"category\":\"%s\",\"schema\":\"%s\","
             "\"tenant_id\":\"%s\",\"task_id\":\"%s\",\"node_id\":\"%s\","
             "\"ts_utc\":\"%s\",\"seq\":%u,\"gseq\":%llu,\"prev_file\":\"%s\"},"
             "\"access\":{\"owner_role\":\"cognition\",\"write_roles\":%s,"
             "\"read_roles\":[\"cognition\"]},\"content\":",
             file_id, category, GW_HALL_SCHEMA, GW_HALL_TENANT, task_id, node_id ? node_id : "",
             ts, seq, (unsigned long long)gseq, prev_file, write_roles);

    size_t json_len = strlen(header) + strlen(content_json) + 2;
    char *json = (char *)AIRY_MALLOC(json_len);
    if (!json) {
        airy_mtx_unlock(&g_hall_lock);
        return -1;
    }
    snprintf(json, json_len, "%s%s}", header, content_json);

    char path[GW_HALL_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, file_id);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        AIRY_FREE(json);
        airy_mtx_unlock(&g_hall_lock);
        AIRY_LOG_WARN("gateway_hall_store: write failed (path=%s)", path);
        return -1;
    }
    fputs(json, fp);
    fclose(fp);
    AIRY_FREE(json);
    airy_mtx_unlock(&g_hall_lock);

#ifndef NDEBUG
    /* 单一真相源事件流断言（与 hall_store.c S-6 对齐，2026-08-20）：
     * 写后必可读。任何已写入事件流的文件必须能立即重新打开并包含自身
     * file.id，否则事件流与持久化不一致（记录丢失）。debug 构建强制
     * 失败，杜绝"写了但读不到"的静默不一致。file.id 位于 header 开头，
     * 读取前 1KB 足以完成校验。 */
    {
        FILE *rf = fopen(path, "r");
        if (rf) {
            char buf[1024];
            size_t got = fread(buf, 1, sizeof(buf) - 1, rf);
            buf[got] = '\0';
            fclose(rf);
            if (strstr(buf, file_id) == NULL) {
                AIRY_LOG_ERROR("gateway_hall_store: invariant violated - write-then-read "
                               "mismatch for %s",
                               file_id);
                return -1;
            }
        } else {
            AIRY_LOG_ERROR("gateway_hall_store: invariant violated - event file missing after "
                           "write (%s)",
                           path);
            return -1;
        }
    }
#endif

    return 0;
}

void gw_hall_task_id_now(char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;
    char ts[GW_HALL_TS_LEN];
    gw_hall_ts_utc(ts, sizeof(ts));
    snprintf(out, out_sz, "gw-%s", ts);
}

/* ── hall watch (read side, SSE push) ─────────────────────────────── */

typedef struct {
    char ts_utc[GW_HALL_TS_LEN];
    unsigned long seq;
    char path[GW_HALL_PATH_MAX];
} gw_hall_cand_t;

static int gw_hall_cand_cmp(const void *a, const void *b)
{
    const gw_hall_cand_t *ca = (const gw_hall_cand_t *)a;
    const gw_hall_cand_t *cb = (const gw_hall_cand_t *)b;
    int c = strcmp(ca->ts_utc, cb->ts_utc);
    if (c != 0)
        return c;
    return (ca->seq > cb->seq) - (ca->seq < cb->seq);
}

static void gw_hall_walk_collect(const char *dir, const char *n, gw_hall_cand_t *cands,
                                 size_t *count, size_t cap)
{
    size_t len = strlen(n);
    if (len < 5 || strcmp(n + len - 5, ".json") != 0)
        return;
    /* filename layout: {tenant}.{task}.{category}.{ts_utc}.{seq}.json
     * (5 dots). tenant/task/category never contain '.', ts_utc is fixed
     * width and seq is digits-only. Parse from the end so variable-length
     * task/category segments don't shift the ts_utc position. */
    const char *last_dot = strrchr(n, '.');
    if (!last_dot || last_dot == n)
        return;
    const char *dot2 = last_dot - 1;
    while (dot2 > n && *dot2 != '.')
        dot2--;
    if (*dot2 != '.')
        return;
    /* seq = segment between dot2 and last_dot */
    unsigned long seq = 0;
    int digits = 0;
    for (const char *q = dot2 + 1; q < last_dot; q++) {
        if (*q < '0' || *q > '9') {
            seq = 0;
            digits = 0;
            break;
        }
        seq = seq * 10 + (unsigned long)(*q - '0');
        digits++;
    }
    if (digits == 0)
        return;
    /* ts_utc = segment before dot2 (variable length up to 24) */
    const char *ts_end = dot2;
    const char *ts_start = ts_end - 1;
    while (ts_start > n && *ts_start != '.')
        ts_start--;
    if (*ts_start != '.')
        return;
    ts_start++;
    size_t tlen = (size_t)(ts_end - ts_start);
    if (tlen == 0 || tlen >= GW_HALL_TS_LEN)
        return;
    char ts[GW_HALL_TS_LEN];
    AIRY_MEMCPY(ts, ts_start, tlen);
    ts[tlen] = '\0';
    if (*count < cap) {
        gw_hall_cand_t *c = &cands[*count];
        AIRY_STRNCPY_TERM(c->ts_utc, ts, sizeof(c->ts_utc));
        c->seq = seq;
        snprintf(c->path, sizeof(c->path), "%s/%s", dir, n);
        (*count)++;
    }
}

/* Recursively scan hall root (tenant/task/category/events.json layout). */
static void gw_hall_watch_walk(const char *dir, gw_hall_cand_t *cands, size_t *count, size_t cap)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char sub[GW_HALL_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            gw_hall_watch_walk(sub, cands, count, cap);
        else
            gw_hall_walk_collect(dir, ent->d_name, cands, count, cap);
    }
    closedir(d);
}

void gw_hall_watch_init(gw_hall_watch_t *w)
{
    if (!w)
        return;
    AIRY_MEMSET(w, 0, sizeof(*w));
    snprintf(w->root, sizeof(w->root), "%s/%s", airy_data_dir(), GW_HALL_ROOT_REL);
    w->initialized = 1;
}

int gw_hall_watch_next(gw_hall_watch_t *w, char *out, size_t out_sz)
{
    if (!w || !out || out_sz == 0)
        return -1;
    if (!w->initialized)
        gw_hall_watch_init(w);

    gw_hall_cand_t cands[4096];
    size_t count = 0;
    gw_hall_watch_walk(w->root, cands, &count, 4096);
    if (count == 0)
        return 0;

    qsort(cands, count, sizeof(cands[0]), gw_hall_cand_cmp);

    for (size_t i = 0; i < count; i++) {
        int gt_ts = strcmp(cands[i].ts_utc, w->last_ts);
        if (gt_ts < 0 || (gt_ts == 0 && cands[i].seq <= w->last_seq))
            continue;
        FILE *fp = fopen(cands[i].path, "r");
        if (!fp)
            continue;
        char *raw = NULL;
        size_t raw_len = 0;
        char chunk[4096];
        size_t got;
        while ((got = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
            char *nr = (char *)AIRY_REALLOC(raw, raw_len + got + 1);
            if (!nr) {
                AIRY_FREE(raw);
                raw = NULL;
                raw_len = 0;
                break;
            }
            raw = nr;
            AIRY_MEMCPY(raw + raw_len, chunk, got);
            raw_len += got;
        }
        fclose(fp);
        if (!raw)
            continue;
        raw[raw_len] = '\0';
        if (raw_len + 1 < out_sz) {
            AIRY_MEMCPY(out, raw, raw_len + 1);
            AIRY_FREE(raw);
            AIRY_STRNCPY_TERM(w->last_ts, cands[i].ts_utc, sizeof(w->last_ts));
            w->last_seq = cands[i].seq;
            return 1;
        }
        AIRY_FREE(raw);
    }
    return 0;
}
