// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_hall_store.c
 * @brief Gateway-side hall event recording (write side) + hall watch (read side).
 *
 * Write side: thin delegation to the single-source-of-truth hall event
 * writer (commons/utils/hall/hall_event.c). The on-disk contract — root
 * airy_data_dir()/agentrt/hall, file naming
 * ({tenant}.{task}.{category}.{ts_utc}.{seq:04u}.json), event body
 * header/access layout, gseq resumption from the disk maximum, prev_file
 * decision-chain linkage, write-role policy, atomic write and the
 * write-then-read assertion — is owned by that one mechanism. The former
 * local copy differed in two ways and was therefore dropped: gseq resumed
 * per process (ordered by scan instead) and the path-traversal check
 * duplicated hall_comp_valid; both now come from the SSoT.
 *
 * Read side: the gateway has no in-process hall handle, so hall watch
 * recursively scans the on-disk event flow, sorts candidates by
 * (ts_utc, seq) and flattens each envelope into the compact wire event
 * shape (hall_event_flatten) shared by hall.replay / hall.stream /
 * hall.watch.
 */

// @owner: team-B
#include "gateway_hall_store.h"

#include "airy_memory.h"
#include "airy_dirent.h"
#include "hall_event.h"
#include "platform.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stat/S_ISDIR for the hall watch read-side directory walk */
#include <sys/stat.h>

#define GW_HALL_PATH_MAX 1024

int gw_hall_store_event(const char *task_id, const char *category, const char *node_id,
                        const char *content_json)
{
    return hall_evt_write(task_id, category, node_id, content_json);
}

void gw_hall_task_id_now(char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;
    char ts[HALL_EVT_TS_LEN];
    hall_clock_utc(ts, sizeof(ts));
    snprintf(out, out_sz, "gw-%s", ts);
}

/* ── hall watch (read side, SSE push) ─────────────────────────────── */

typedef struct {
    char ts_utc[HALL_EVT_TS_LEN];
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
    hall_evt_parts_t parts;
    if (hall_evt_parse(n, &parts) != 0)
        return;
    if (parts.ts_utc_len >= HALL_EVT_TS_LEN)
        return;
    unsigned long seq = hall_evt_seq(n);
    if (seq == 0 || *count >= cap)
        return;
    gw_hall_cand_t *c = &cands[*count];
    AIRY_MEMCPY(c->ts_utc, parts.ts_utc, parts.ts_utc_len);
    c->ts_utc[parts.ts_utc_len] = '\0';
    c->seq = seq;
    snprintf(c->path, sizeof(c->path), "%s/%s", dir, n);
    (*count)++;
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
    snprintf(w->root, sizeof(w->root), "%s/%s", airy_data_dir(), HALL_EVT_ROOT_REL);
    w->initialized = 1;
}

/* Read a whole file into a malloc'd buffer (caller AIRY_FREE). NULL on error. */
static char *gw_hall_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)AIRY_MALLOC((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        AIRY_FREE(buf);
        return NULL;
    }
    buf[got] = '\0';
    return buf;
}

/* Flatten one stored event envelope into the compact on-the-wire event
 * shape shared by every hall.* read endpoint (see header). */
char *hall_event_flatten(const char *path)
{
    if (!path)
        return NULL;
    char *raw = gw_hall_read_file(path);
    if (!raw)
        return NULL;
    cJSON *o = cJSON_Parse(raw);
    AIRY_FREE(raw);
    if (!o)
        return NULL;

    cJSON *f = cJSON_GetObjectItem(o, "file");
    const char *file_id = f ? cJSON_GetStringValue(cJSON_GetObjectItem(f, "id")) : NULL;
    const char *category = f ? cJSON_GetStringValue(cJSON_GetObjectItem(f, "category")) : NULL;
    const char *task_id = f ? cJSON_GetStringValue(cJSON_GetObjectItem(f, "task_id")) : NULL;
    const char *tenant_id = f ? cJSON_GetStringValue(cJSON_GetObjectItem(f, "tenant_id")) : NULL;
    const char *node_id = f ? cJSON_GetStringValue(cJSON_GetObjectItem(f, "node_id")) : NULL;
    const char *ts_utc = f ? cJSON_GetStringValue(cJSON_GetObjectItem(f, "ts_utc")) : NULL;
    double seq = f ? cJSON_GetNumberValue(cJSON_GetObjectItem(f, "seq")) : 0;
    double gseq = f ? cJSON_GetNumberValue(cJSON_GetObjectItem(f, "gseq")) : 0;
    cJSON *content = cJSON_GetObjectItem(o, "content");

    cJSON *evt = cJSON_CreateObject();
    if (evt) {
        cJSON_AddStringToObject(evt, "file_id", file_id ? file_id : "");
        cJSON_AddStringToObject(evt, "category", category ? category : "");
        cJSON_AddStringToObject(evt, "task_id", task_id ? task_id : "");
        cJSON_AddStringToObject(evt, "tenant_id", tenant_id ? tenant_id : "");
        cJSON_AddStringToObject(evt, "node_id", node_id ? node_id : "");
        cJSON_AddStringToObject(evt, "ts_utc", ts_utc ? ts_utc : "");
        cJSON_AddNumberToObject(evt, "seq", seq);
        cJSON_AddNumberToObject(evt, "gseq", gseq);
        if (content && cJSON_IsObject(content))
            cJSON_AddItemToObject(evt, "content", cJSON_Duplicate(content, 1));
        else
            cJSON_AddItemToObject(evt, "content", cJSON_CreateObject());
    }
    cJSON_Delete(o);
    if (!evt)
        return NULL;
    char *s = cJSON_PrintUnformatted(evt);
    cJSON_Delete(evt);
    return s;
}

int gw_hall_watch_next(gw_hall_watch_t *w, char *out, size_t out_sz)
{
    if (!w || !out || out_sz == 0)
        return -1;
    if (!w->initialized)
        gw_hall_watch_init(w);

    /* cands 在堆上分配：每个条目约 1KB，4096 条约 4MB，不能放线程栈。 */
    const size_t cand_cap = 4096;
    gw_hall_cand_t *cands = (gw_hall_cand_t *)AIRY_CALLOC(cand_cap, sizeof(gw_hall_cand_t));
    if (!cands)
        return -1;
    size_t count = 0;
    gw_hall_watch_walk(w->root, cands, &count, cand_cap);
    if (count == 0) {
        AIRY_FREE(cands);
        return 0;
    }

    qsort(cands, count, sizeof(cands[0]), gw_hall_cand_cmp);

    int result = 0;
    for (size_t i = 0; i < count; i++) {
        int gt_ts = strcmp(cands[i].ts_utc, w->last_ts);
        if (gt_ts < 0 || (gt_ts == 0 && cands[i].seq <= w->last_seq))
            continue;
        /* 推送形态与 hall.stream/replay 的 events 条目一致（SSoT）：
         * 展开 envelope 为扁平事件，订阅方用同一解码器消费两路数据。 */
        char *flat = hall_event_flatten(cands[i].path);
        if (!flat)
            continue;
        size_t fl = strlen(flat);
        if (fl + 1 <= out_sz) {
            AIRY_MEMCPY(out, flat, fl + 1);
            AIRY_FREE(flat);
            AIRY_STRNCPY_TERM(w->last_ts, cands[i].ts_utc, sizeof(w->last_ts));
            w->last_seq = cands[i].seq;
            result = 1;
            break;
        }
        AIRY_FREE(flat);
    }
    AIRY_FREE(cands);
    return result;
}
