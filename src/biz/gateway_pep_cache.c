// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gateway_pep_cache.c
 * @brief Gateway PEP 裁定缓存实现（M2-S5，0.1.9 §3.2）。
 *
 * 缓存键：(agent, tool, action) 三元组（FNV-1a 哈希 + 逐字段比对）。
 * 失效键：epoch——每次 miss RPC 返回权威 epoch，与本地已见 epoch 不等
 * 即整体失效（策略版本 SSoT，epoch 单调递增由 PDP 保证）。
 *
 * 线程安全：airy_mtx 互斥保护缓存表；RPC 在锁外执行（不阻塞其他
 * 请求的缓存命中路径）。
 */

#include "gateway_pep_cache.h"

#include "airy_memory.h"
#include "daemon_security.h"
#include "logging.h"

#include "platform_sync.h"

#include <cjson/cJSON.h>

#include <string.h>

#define GW_PEP_MAX 256
#define GW_PEP_RPC_TIMEOUT_MS 5000
#define GW_PEP_AGENT_LEN 64
#define GW_PEP_TOOL_LEN 64
#define GW_PEP_ACTION_LEN 32

typedef struct {
    uint32_t hash;
    uint64_t epoch;
    int allowed; /* 0=allow, -1=deny */
    int used;
    char agent[GW_PEP_AGENT_LEN];
    char tool[GW_PEP_TOOL_LEN];
    char action[GW_PEP_ACTION_LEN];
} gw_pep_entry_t;

static gw_pep_entry_t s_pep[GW_PEP_MAX];
static uint64_t s_epoch;
static airy_mtx_t s_lock;
static int s_inited;

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t key_hash(const char *a, const char *t, const char *ac)
{
    uint32_t h = fnv1a(a);
    h = h * 33u + fnv1a(t);
    h = h * 33u + fnv1a(ac);
    return h;
}

static void lock_pep(void)
{
    if (!s_inited) {
        if (airy_mtx_init(&s_lock) != 0)
            return;
        s_inited = 1;
    }
    airy_mtx_lock(&s_lock);
}

static void unlock_pep(void)
{
    airy_mtx_unlock(&s_lock);
}

void gw_pep_init(void)
{
    if (s_inited)
        return;
    if (airy_mtx_init(&s_lock) != 0)
        return;
    s_inited = 1;
    s_epoch = 0;
}

void gw_pep_clear(void)
{
    lock_pep();
    __builtin_memset(s_pep, 0, sizeof(s_pep));
    s_epoch = 0;
    unlock_pep();
}

uint64_t gw_pep_epoch(void)
{
    return s_epoch;
}

/* 降级路径：PDP 不可达时回退本地 ACL（保持既有 fail-closed 语义） */
static int fallback_acl(const char *agent, const char *tool, const char *action)
{
    int rc = daemon_check_tool_permission(agent, tool, action);
    if (rc != 0)
        AIRY_LOG_WARN("gateway PEP fallback ACL DENY: agent=%s tool=%s action=%s", agent, tool,
                      action);
    return rc == 0 ? 0 : -1;
}

/* 锁内查找命中条目（epoch 一致），返回索引；未命中返回 -1 */
static int lookup_locked(uint32_t h, const char *a, const char *t, const char *ac)
{
    for (size_t i = 0; i < GW_PEP_MAX; i++) {
        const gw_pep_entry_t *e = &s_pep[i];
        if (!e->used || e->hash != h)
            continue;
        if (strcmp(e->agent, a) != 0 || strcmp(e->tool, t) != 0 ||
            strcmp(e->action, ac) != 0)
            continue;
        if (e->epoch != s_epoch)
            continue; /* epoch 过期条目视作 miss */
        return (int)i;
    }
    return -1;
}

/* 锁内存储（线性探测找空槽；满则整体清空后存首槽） */
static void store_locked(uint32_t h, const char *a, const char *t, const char *ac, int allowed)
{
    size_t slot = h % GW_PEP_MAX;
    for (size_t i = 0; i < GW_PEP_MAX; i++) {
        size_t idx = (slot + i) % GW_PEP_MAX;
        if (!s_pep[idx].used) {
            gw_pep_entry_t *e = &s_pep[idx];
            __builtin_memset(e, 0, sizeof(*e));
            e->hash = h;
            e->epoch = s_epoch;
            e->allowed = allowed;
            e->used = 1;
            AIRY_STRNCPY_TERM(e->agent, a, sizeof(e->agent));
            AIRY_STRNCPY_TERM(e->tool, t, sizeof(e->tool));
            AIRY_STRNCPY_TERM(e->action, ac, sizeof(e->action));
            return;
        }
    }
    /* 缓存满：整体失效重来（epoch 已对齐，极端情况可接受） */
    __builtin_memset(s_pep, 0, sizeof(s_pep));
    gw_pep_entry_t *e = &s_pep[0];
    e->hash = h;
    e->epoch = s_epoch;
    e->allowed = allowed;
    e->used = 1;
    AIRY_STRNCPY_TERM(e->agent, a, sizeof(e->agent));
    AIRY_STRNCPY_TERM(e->tool, t, sizeof(e->tool));
    AIRY_STRNCPY_TERM(e->action, ac, sizeof(e->action));
}

/* RPC 到 PDP 请求裁定；返回 0=allow / -1=deny / -2=PDP 不可达 */
static int rpc_judge(const gateway_business_ctx_t *ctx, const char *agent, const char *tool,
                     const char *action, uint64_t *out_epoch)
{
    if (!ctx || !ctx->cupolas_sock_path[0])
        return -2;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -2;
    cJSON_AddStringToObject(params, "agent_id", agent);
    cJSON_AddStringToObject(params, "action", action);
    cJSON_AddStringToObject(params, "resource", tool);
    cJSON_AddNumberToObject(params, "epoch_hint", (double)s_epoch);
    char *ps = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!ps)
        return -2;

    char *resp = gw_svc_call(ctx->cupolas_sock_path, "check_permission", ps,
                             GW_PEP_RPC_TIMEOUT_MS);
    AIRY_FREE(ps);
    if (!resp)
        return -2;

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -2;
    cJSON *res = cJSON_GetObjectItem(root, "result");
    int allowed = 0;
    uint64_t epoch = 0;
    if (cJSON_IsObject(res)) {
        cJSON *al = cJSON_GetObjectItem(res, "allowed");
        if (cJSON_IsBool(al))
            allowed = cJSON_IsTrue(al) ? 1 : 0;
        cJSON *ep = cJSON_GetObjectItem(res, "epoch");
        if (cJSON_IsNumber(ep))
            epoch = (uint64_t)ep->valuedouble;
    }
    cJSON_Delete(root);
    *out_epoch = epoch;
    return allowed ? 0 : -1;
}

int gw_pep_check(const gateway_business_ctx_t *ctx, const char *agent, const char *tool,
                 const char *action)
{
    if (!agent || !tool || !action || !agent[0] || !tool[0] || !action[0])
        return -1;

    gw_pep_init();
    uint32_t h = key_hash(agent, tool, action);

    /* 1. 缓存命中（epoch 一致，零 RPC） */
    lock_pep();
    int idx = lookup_locked(h, agent, tool, action);
    if (idx >= 0) {
        int allowed = s_pep[idx].allowed;
        unlock_pep();
        return allowed;
    }
    unlock_pep();

    /* 2. miss：向 PDP 请求裁定（锁外 RPC，不阻塞其他命中路径） */
    uint64_t rpc_epoch = 0;
    int verdict = rpc_judge(ctx, agent, tool, action, &rpc_epoch);
    if (verdict == -2) {
        /* PDP 不可达：降级本地 ACL（fail-closed 语义保持） */
        return fallback_acl(agent, tool, action);
    }

    /* 3. epoch 对齐：权威 epoch 变化 → 整体失效后重填 */
    lock_pep();
    if (rpc_epoch != s_epoch) {
        __builtin_memset(s_pep, 0, sizeof(s_pep));
        s_epoch = rpc_epoch;
    }
    store_locked(h, agent, tool, action, verdict);
    unlock_pep();
    return verdict;
}
