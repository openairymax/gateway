// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_hall.c
 * @brief Gateway hall.* namespace: task board / event stream / decision chain.
 *
 * Unlike the other namespaces (pure <ns>.<method> forwarding to a daemon),
 * hall.* is implemented directly inside the gateway: it reads the persisted
 * single-source-of-truth files that the runtime writes under AIRY_HOME —
 *   - work hall board : $AIRY_HOME/data/agentrt/state/work_hall_state.json
 *   - hall store      : $AIRY_HOME/data/agentrt/hall/<tenant>/<task>/<cat>/events.json
 * and merges in the live agent roster from agent_d (agent.list). Any client
 * (Rust TUI / C CLI / HTTP) therefore sees the same board / event stream /
 * decision chain without owning the in-process hall handle.
 *
 * Event ordering: files are named {tenant}.{task}.{category}.{ts_utc}.{seq}.json;
 * within one writer process gseq gives the true causal order, but across
 * processes gseq restarts, so the stable global order is (ts_utc, seq)
 * (ts_utc is UTC YYYYMMDDTHHMMSSmmm, fixed-width, lexicographically ordered).
 *
 * Methods:
 *   hall.board   ()                        -> {entries, agents}
 *   hall.tasks   ({limit?})                -> {tasks, total}
 *   hall.replay  ({task_id, category?})    -> {task_id, events, total}
 *   hall.stream  ({limit?})                -> {events, total}
 */

#include "gateway_biz_internal.h"

#include "syscalls.h"

#include "logging.h"
#include "platform.h"
#include "airy_dirent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW_HALL_STATE_REL "agentrt/state/work_hall_state.json"
#define GW_HALL_ROOT_REL "agentrt/hall"
#define GW_HALL_DEFAULT_STREAM_LIMIT 512
#define GW_HALL_MAX_LIMIT 8192
#define GW_HALL_PATH_MAX 1024
#define GW_HALL_FILE_ID_MAX 192
#define GW_HALL_TS_LEN 32

/* ── file I/O helpers ─────────────────────────────────────────────── */

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
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)AIRY_MALLOC((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

static void gw_hall_state_path(char *buf, size_t cap)
{
    /* 收敛到 data/agentrt/state（AIRY_HOME 路径体系），与写入方
     * work_hall_persist.c 一致。 */
    const char *data = airy_data_dir();
    if (data && data[0])
        snprintf(buf, cap, "%s/%s", data, GW_HALL_STATE_REL);
    else
        snprintf(buf, cap, "%s", GW_HALL_STATE_REL);
}

static void gw_hall_root(char *buf, size_t cap)
{
    const char *data = airy_data_dir();
    if (data && data[0])
        snprintf(buf, cap, "%s/%s", data, GW_HALL_ROOT_REL);
    else
        snprintf(buf, cap, "%s", GW_HALL_ROOT_REL);
}

/* ── hall file-name parsing ─────────────────────────────────────────
 * Naming: {tenant}.{task}.{category}.{ts_utc}.{seq:04d}.json
 * BAN-154: sscanf forbidden; split manually on '.' (tenant/task/category
 * never contain '.'; ts_utc is a fixed-width digit string).
 * out fields are filled with the substrings (pointers into name). */
typedef struct {
    const char *tenant;
    size_t tenant_len;
    const char *task;
    size_t task_len;
    const char *category;
    size_t category_len;
    const char *ts_utc;
    size_t ts_utc_len;
    const char *seq;
    size_t seq_len;
} gw_hall_name_parts_t;

static int gw_hall_parse_name(const char *name, gw_hall_name_parts_t *out)
{
    if (!name || !out)
        return -1;
    size_t len = strlen(name);
    /* minimum: a.b.c.ts.seq.json -> at least 5 dots */
    const char *dot[5];
    int n = 0;
    for (size_t i = 0; i < len && n < 5; i++) {
        if (name[i] == '.')
            dot[n++] = name + i;
    }
    if (n != 5)
        return -1;
    /* trailing ".json" */
    if (strcmp(dot[4], ".json") != 0)
        return -1;

    out->tenant = name;
    out->tenant_len = (size_t)(dot[0] - name);
    out->task = dot[0] + 1;
    out->task_len = (size_t)(dot[1] - dot[0] - 1);
    out->category = dot[1] + 1;
    out->category_len = (size_t)(dot[2] - dot[1] - 1);
    out->ts_utc = dot[2] + 1;
    out->ts_utc_len = (size_t)(dot[3] - dot[2] - 1);
    out->seq = dot[3] + 1;
    out->seq_len = (size_t)(dot[4] - dot[3] - 1);
    if (!out->tenant_len || !out->task_len || !out->category_len || !out->ts_utc_len ||
        !out->seq_len)
        return -1;
    return 0;
}

static int gw_hall_ends_with_json(const char *name)
{
    size_t l = name ? strlen(name) : 0;
    return l >= 5 && strcmp(name + l - 5, ".json") == 0;
}

/* ── growable event array ─────────────────────────────────────────── */

typedef struct {
    char ts_utc[GW_HALL_TS_LEN]; /* sort key 1 (fixed width, lexical) */
    unsigned long seq;           /* sort key 2 */
    char *json;                  /* compact event JSON (OWNER) */
} gw_hall_evt_t;

typedef struct {
    gw_hall_evt_t *items;
    size_t count;
    size_t cap;
} gw_hall_evt_list_t;

static int gw_hall_evt_reserve(gw_hall_evt_list_t *l, size_t extra)
{
    if (l->count + extra <= l->cap)
        return 0;
    size_t ncap = l->cap ? l->cap * 2 : 64;
    while (ncap < l->count + extra)
        ncap *= 2;
    gw_hall_evt_t *ni = (gw_hall_evt_t *)AIRY_REALLOC(l->items, ncap * sizeof(*ni));
    if (!ni)
        return -1;
    l->items = ni;
    l->cap = ncap;
    return 0;
}

static void gw_hall_evt_push(gw_hall_evt_list_t *l, const char *ts_utc, unsigned long seq,
                             char *json)
{
    if (gw_hall_evt_reserve(l, 1) != 0) {
        AIRY_FREE(json);
        return;
    }
    gw_hall_evt_t *e = &l->items[l->count++];
    AIRY_STRNCPY_TERM(e->ts_utc, ts_utc ? ts_utc : "", sizeof(e->ts_utc));
    e->seq = seq;
    e->json = json;
}

/* insertion sort by (ts_utc, seq) — hall event counts are small (KBs) */
static void gw_hall_evt_sort(gw_hall_evt_list_t *l)
{
    for (size_t i = 1; i < l->count; i++) {
        gw_hall_evt_t key = l->items[i];
        size_t j = i;
        while (j > 0) {
            int c = strcmp(l->items[j - 1].ts_utc, key.ts_utc);
            if (c > 0 || (c == 0 && l->items[j - 1].seq > key.seq))
                j--;
            else
                break;
        }
        if (j != i) {
            for (size_t k = i; k > j; k--)
                l->items[k] = l->items[k - 1];
            l->items[j] = key;
        }
    }
}

static void gw_hall_evt_free_all(gw_hall_evt_list_t *l)
{
    for (size_t i = 0; i < l->count; i++)
        AIRY_FREE(l->items[i].json);
    AIRY_FREE(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

/* Build the compact event JSON from a raw hall file. Returns a malloc'd
 * string on success (caller AIRY_FREE), NULL when the file is not parseable. */
static char *gw_hall_event_from_file(const char *path)
{
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

/* Scan one hall file: parse its name + build the event. Returns 0 when the
 * file contributed an event, -1 otherwise (skipped). */
static int gw_hall_collect_file(const char *path, const char *name, gw_hall_evt_list_t *out)
{
    gw_hall_name_parts_t parts;
    if (gw_hall_parse_name(name, &parts) != 0)
        return -1;
    char ts_utc[GW_HALL_TS_LEN];
    size_t tlen = parts.ts_utc_len;
    if (tlen >= sizeof(ts_utc))
        tlen = sizeof(ts_utc) - 1;
    AIRY_MEMCPY(ts_utc, parts.ts_utc, tlen);
    ts_utc[tlen] = '\0';

    unsigned long seq = 0;
    for (size_t i = 0; i < parts.seq_len; i++)
        seq = seq * 10 + (unsigned long)(parts.seq[i] - '0');

    char *json = gw_hall_event_from_file(path);
    if (!json)
        return -1;
    gw_hall_evt_push(out, ts_utc, seq, json);
    return 0;
}

/* Recursively scan the hall root. Directory layout:
 *   <root>/<tenant>/<task>/<category>/events.json   (depth 0=root,1=tenant,2=task,3=cat)
 * `tenant_filter`/`task_filter`/`cat_filter` are optional exact matches
 * (NULL = any); files are collected into `out`. */
static void gw_hall_walk(const char *dir, int depth, const char *tenant_filter,
                         const char *task_filter, const char *cat_filter, gw_hall_evt_list_t *out)
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
        if (depth == 3 && gw_hall_ends_with_json(ent->d_name)) {
            gw_hall_collect_file(sub, ent->d_name, out);
            continue;
        }
        /* depth 0/1/2: subdirectories (tenant/task/category) */
        if ((depth == 0 && tenant_filter && strcmp(ent->d_name, tenant_filter) != 0) ||
            (depth == 1 && task_filter && strcmp(ent->d_name, task_filter) != 0) ||
            (depth == 2 && cat_filter && strcmp(ent->d_name, cat_filter) != 0))
            continue;
        gw_hall_walk(sub, depth + 1, tenant_filter, task_filter, cat_filter, out);
    }
    closedir(d);
}

/* ── method implementations ───────────────────────────────────────── */

static char *gw_hall_board(const gateway_business_ctx_t *ctx, const cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    cJSON_AddItemToObject(result, "entries", entries);
    cJSON_AddStringToObject(result, "source", GW_HALL_STATE_REL);

    char state_path[GW_HALL_PATH_MAX];
    gw_hall_state_path(state_path, sizeof(state_path));
    char *raw = gw_hall_read_file(state_path);
    if (raw) {
        cJSON *state = cJSON_Parse(raw);
        AIRY_FREE(raw);
        if (state) {
            cJSON *board = cJSON_GetObjectItem(state, "board");
            if (cJSON_IsArray(board)) {
                int n = cJSON_GetArraySize(board);
                for (int i = 0; i < n; i++) {
                    cJSON *e = cJSON_GetArrayItem(board, i);
                    cJSON *dup = cJSON_Duplicate(e, 1);
                    if (dup)
                        cJSON_AddItemToArray(entries, dup);
                }
            }
            cJSON_Delete(state);
        }
    }

    /* Live agent roster from agent_d (best effort; failure -> empty list) */
    cJSON *agents = cJSON_CreateArray();
    cJSON_AddItemToObject(result, "agents", agents);
    if (ctx) {
        /* 架构约束 2026-08-25 "必须走 syscall": agent.list 经 SYS_SVC_CALL 派发 */
        char *resp = NULL;
        airy_err_t rc = airy_sys_svc_call("agent", "list", "{}", GW_TOOL_TIMEOUT_MS, &resp);
        if (rc == AIRY_SUCCESS && resp) {
            cJSON *r = cJSON_Parse(resp);
            AIRY_FREE(resp);
            if (r) {
                cJSON *res = cJSON_GetObjectItem(r, "result");
                if (cJSON_IsObject(res)) {
                    cJSON *ids = cJSON_GetObjectItem(res, "agent_ids");
                    if (cJSON_IsArray(ids)) {
                        int n = cJSON_GetArraySize(ids);
                        for (int i = 0; i < n; i++) {
                            cJSON *it = cJSON_GetArrayItem(ids, i);
                            if (cJSON_IsString(it))
                                cJSON_AddItemToArray(agents, cJSON_Duplicate(it, 1));
                        }
                    }
                    cJSON *total = cJSON_GetObjectItem(res, "total");
                    if (cJSON_IsNumber(total))
                        cJSON_AddNumberToObject(result, "agent_total", total->valuedouble);
                }
                cJSON_Delete(r);
            }
        }
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id))
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    else
        cJSON_AddNullToObject(out, "id");
    cJSON_AddItemToObject(out, "result", result);
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s;
}

static char *gw_hall_tasks(const cJSON *params, const cJSON *id)
{
    char root[GW_HALL_PATH_MAX];
    gw_hall_root(root, sizeof(root));

    /* task map: task_id -> {tenant, latest_ts, event_count} */
    typedef struct {
        char task[128];
        char tenant[64];
        char latest_ts[GW_HALL_TS_LEN];
        size_t event_count;
    } gw_task_t;
    gw_task_t *tasks = NULL;
    size_t nt = 0, cap = 0;

    /* walk tenant -> task dirs directly (files are one level deeper) */
    DIR *d = opendir(root);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;
            char tdir[GW_HALL_PATH_MAX];
            snprintf(tdir, sizeof(tdir), "%s/%s", root, ent->d_name);
            DIR *td = opendir(tdir);
            if (!td)
                continue;
            struct dirent *tent;
            while ((tent = readdir(td)) != NULL) {
                if (tent->d_name[0] == '.')
                    continue;
                /* <task>/<category>/<file>.json */
                char catdir[GW_HALL_PATH_MAX];
                snprintf(catdir, sizeof(catdir), "%s/%s", tdir, tent->d_name);
                DIR *cd = opendir(catdir);
                if (!cd)
                    continue;
                struct dirent *c;
                while ((c = readdir(cd)) != NULL) {
                    if (c->d_name[0] == '.')
                        continue;
                    char filedir[GW_HALL_PATH_MAX];
                    snprintf(filedir, sizeof(filedir), "%s/%s", catdir, c->d_name);
                    DIR *fd = opendir(filedir);
                    if (!fd)
                        continue;
                    struct dirent *f;
                    while ((f = readdir(fd)) != NULL) {
                        if (f->d_name[0] == '.' || !gw_hall_ends_with_json(f->d_name))
                            continue;
                        gw_hall_name_parts_t parts;
                        if (gw_hall_parse_name(f->d_name, &parts) != 0)
                            continue;
                        /* find or create the task entry */
                        size_t idx = nt;
                        for (size_t i = 0; i < nt; i++) {
                            if (strcmp(tasks[i].task, tent->d_name) == 0) {
                                idx = i;
                                break;
                            }
                        }
                        if (idx == nt) {
                            if (nt == cap) {
                                cap = cap ? cap * 2 : 16;
                                gw_task_t *nta =
                                    (gw_task_t *)AIRY_REALLOC(tasks, cap * sizeof(*tasks));
                                if (!nta) {
                                    closedir(fd);
                                    closedir(cd);
                                    closedir(td);
                                    closedir(d);
                                    goto tasks_done;
                                }
                                tasks = nta;
                            }
                            tasks[nt].latest_ts[0] = '\0';
                            tasks[nt].event_count = 0;
                            AIRY_STRNCPY_TERM(tasks[nt].task, tent->d_name,
                                              sizeof(tasks[nt].task));
                            AIRY_STRNCPY_TERM(tasks[nt].tenant, ent->d_name,
                                              sizeof(tasks[nt].tenant));
                            nt++;
                        }
                        tasks[idx].event_count++;
                        char ts[GW_HALL_TS_LEN];
                        size_t tlen = parts.ts_utc_len;
                        if (tlen >= sizeof(ts))
                            tlen = sizeof(ts) - 1;
                        AIRY_MEMCPY(ts, parts.ts_utc, tlen);
                        ts[tlen] = '\0';
                        if (strcmp(ts, tasks[idx].latest_ts) > 0)
                            AIRY_STRNCPY_TERM(tasks[idx].latest_ts, ts,
                                              sizeof(tasks[idx].latest_ts));
                    }
                    closedir(fd);
                }
                closedir(cd);
            }
            closedir(td);
        }
        closedir(d);
    }

tasks_done:
    /* newest first */
    for (size_t i = 1; i < nt; i++) {
        gw_task_t key = tasks[i];
        size_t j = i;
        while (j > 0 && strcmp(tasks[j - 1].latest_ts, key.latest_ts) < 0)
            j--;
        if (j != i) {
            for (size_t k = i; k > j; k--)
                tasks[k] = tasks[k - 1];
            tasks[j] = key;
        }
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < nt; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "tenant_id", tasks[i].tenant);
        cJSON_AddStringToObject(t, "task_id", tasks[i].task);
        cJSON_AddStringToObject(t, "latest_ts", tasks[i].latest_ts);
        cJSON_AddNumberToObject(t, "event_count", (double)tasks[i].event_count);
        cJSON_AddItemToArray(arr, t);
    }
    AIRY_FREE(tasks);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tasks", arr);
    cJSON_AddNumberToObject(result, "total", (double)nt);
    cJSON_AddStringToObject(result, "root", root);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id))
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    else
        cJSON_AddNullToObject(out, "id");
    cJSON_AddItemToObject(out, "result", result);
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s;
}

static char *gw_hall_replay(const cJSON *params, const cJSON *id)
{
    const char *task_id = NULL;
    const char *category = NULL;
    if (params) {
        cJSON *t = cJSON_GetObjectItem(params, "task_id");
        if (cJSON_IsString(t) && t->valuestring && *t->valuestring)
            task_id = t->valuestring;
        cJSON *c = cJSON_GetObjectItem(params, "category");
        if (cJSON_IsString(c) && c->valuestring && *c->valuestring)
            category = c->valuestring;
    }
    if (!task_id) {
        return jsonrpc_error(-32602, "Invalid params: missing task_id", id);
    }

    char root[GW_HALL_PATH_MAX];
    gw_hall_root(root, sizeof(root));

    gw_hall_evt_list_t evts = {0};
    gw_hall_walk(root, 0, NULL, task_id, category, &evts);
    gw_hall_evt_sort(&evts);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < evts.count; i++) {
        cJSON *e = cJSON_Parse(evts.items[i].json);
        if (e) {
            cJSON_AddItemToArray(arr, e);
        }
    }
    size_t total = evts.count;
    gw_hall_evt_free_all(&evts);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "task_id", task_id);
    cJSON_AddItemToObject(result, "events", arr);
    cJSON_AddNumberToObject(result, "total", (double)total);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id))
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    else
        cJSON_AddNullToObject(out, "id");
    cJSON_AddItemToObject(out, "result", result);
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s;
}

static char *gw_hall_stream(const cJSON *params, const cJSON *id)
{
    size_t limit = GW_HALL_DEFAULT_STREAM_LIMIT;
    if (params) {
        cJSON *l = cJSON_GetObjectItem(params, "limit");
        if (cJSON_IsNumber(l) && l->valuedouble > 0) {
            limit = (size_t)l->valuedouble;
            if (limit > GW_HALL_MAX_LIMIT)
                limit = GW_HALL_MAX_LIMIT;
        }
    }

    char root[GW_HALL_PATH_MAX];
    gw_hall_root(root, sizeof(root));

    gw_hall_evt_list_t evts = {0};
    gw_hall_walk(root, 0, NULL, NULL, NULL, &evts);
    gw_hall_evt_sort(&evts);

    size_t take = evts.count < limit ? evts.count : limit;
    size_t start = evts.count - take; /* newest `limit` entries */

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = start; i < evts.count; i++) {
        cJSON *e = cJSON_Parse(evts.items[i].json);
        if (e)
            cJSON_AddItemToArray(arr, e);
    }
    size_t total = evts.count;
    gw_hall_evt_free_all(&evts);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "events", arr);
    cJSON_AddNumberToObject(result, "total", (double)total);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id))
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    else
        cJSON_AddNullToObject(out, "id");
    cJSON_AddItemToObject(out, "result", result);
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s;
}

/**
 * @brief hall.* method dispatch (implemented in the gateway itself).
 */
char *handle_hall_call(cJSON *root, gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method) || !method->valuestring)
        return jsonrpc_error(-32600, "Invalid Request", id);

    if (strcmp(method->valuestring, "hall.board") == 0)
        return gw_hall_board(ctx, id);
    if (strcmp(method->valuestring, "hall.tasks") == 0)
        return gw_hall_tasks(params, id);
    if (strcmp(method->valuestring, "hall.replay") == 0)
        return gw_hall_replay(params, id);
    if (strcmp(method->valuestring, "hall.stream") == 0)
        return gw_hall_stream(params, id);

    return jsonrpc_error(-32601, "Method not found", id);
}
