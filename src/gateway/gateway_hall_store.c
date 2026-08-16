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

/* Next seq for (task, category): max existing seq + 1, so concurrent writer
 * processes (C CLI / gateway) never overwrite each other's event files. */
static unsigned gw_hall_next_seq(const char *dir)
{
    unsigned max_seq = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        size_t len = strlen(n);
        if (len < 5 || strcmp(n + len - 5, ".json") != 0)
            continue;
        const char *dot = NULL;
        for (const char *p = n; *p; p++) {
            if (*p == '.')
                dot = p;
        }
        if (!dot)
            continue;
        unsigned seq = 0;
        int digits = 0;
        for (const char *p = dot + 1; p < n + len - 5; p++) {
            if (*p < '0' || *p > '9') {
                seq = 0;
                digits = 0;
                break;
            }
            seq = seq * 10 + (unsigned)(*p - '0');
            digits++;
        }
        if (digits > 0 && seq > max_seq)
            max_seq = seq;
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

    unsigned seq = gw_hall_next_seq(dir);

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
             "\"ts_utc\":\"%s\",\"seq\":%u,\"gseq\":%llu,\"prev_file\":\"\"},"
             "\"access\":{\"owner_role\":\"cognition\",\"write_roles\":%s,"
             "\"read_roles\":[\"cognition\"]},\"content\":",
             file_id, category, GW_HALL_SCHEMA, GW_HALL_TENANT, task_id, node_id ? node_id : "",
             ts, seq, (unsigned long long)gseq, write_roles);

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
