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

#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

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

/* ── M2-S4 epoch 主动失效（0.1.9 §3.2/§3.3.1） ────────────────────
 * gateway 订阅 notify_d topic=airy.cupolas.epoch：策略热更新（activate/
 * rollback，epoch+1 + 广播）到达时主动整体失效缓存，无需等下次 miss
 * RPC 才对齐——保证全 runtime 生效 < 1s（订阅面 fail-open：notify_d
 * 不在线仅告警重连，不影响既有懒对齐路径）。 */

/* 从 notify_d SSE data 帧解析权威 epoch。
 * notify_d 广播帧形如 {"event":"epoch_change","topic":"airy.cupolas.epoch",
 * "message":"{"epoch":N}"}——message 内层 JSON 未经转义，整帧并非严格
 * 合法 JSON，故按 topic 前缀 + "epoch":N 键值解析（容错），
 * 非本 topic 的帧返回 0。 */
uint64_t gw_pep_epoch_parse(const char *data)
{
    if (!data || !strstr(data, "airy.cupolas.epoch"))
        return 0;
    const char *key = strstr(data, "\"epoch\":");
    if (!key)
        return 0;
    key += strlen("\"epoch\":");
    while (*key == ' ' || *key == '\t')
        key++;
    if (*key < '0' || *key > '9')
        return 0;
    return strtoull(key, NULL, 10);
}

/* 采用订阅面下发的权威 epoch：单调推进才整体失效（幂等，旧值忽略） */
static void adopt_epoch(uint64_t epoch)
{
    if (epoch == 0)
        return;
    lock_pep();
    if (epoch > s_epoch) {
        __builtin_memset(s_pep, 0, sizeof(s_pep));
        s_epoch = epoch;
    }
    unlock_pep();
}

#ifndef _WIN32
typedef struct {
    char path[256];
} epoch_watch_arg_t;

/* notify_d SSE 长连接订阅线程（detached）：读帧 → 解析 → 主动失效。
 * 断线/无活动超时自动重连（尽力而为，fail-open）。 */
static void *epoch_watch_main(void *arg)
{
    epoch_watch_arg_t *a = (epoch_watch_arg_t *)arg;
    if (!a) {
        AIRY_LOG_WARN("gateway PEP: epoch watch arg lost, observer stopped");
        return NULL;
    }

    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            AIRY_LOG_WARN("gateway PEP: epoch watch socket() failed, retry in 2s");
            sleep(2);
            continue;
        }
        struct sockaddr_un addr;
        __builtin_memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", a->path);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            AIRY_LOG_WARN("gateway PEP: notify_d unreachable (%s), retry in 2s", a->path);
            close(fd);
            sleep(2);
            continue;
        }

        /* SSE 握手：notify_d 将本连接注册为长连接订阅客户端 */
        char hdr[512];
        int hl = snprintf(hdr, sizeof(hdr),
                          "GET /events HTTP/1.1\r\n"
                          "Accept: text/event-stream\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Connection: keep-alive\r\n"
                          "X-Client-Id: gateway-pep\r\n"
                          "\r\n");
        if (hl <= 0 || hl >= (int)sizeof(hdr) ||
            send(fd, hdr, (size_t)hl, 0) <= 0) {
            close(fd);
            sleep(2);
            continue;
        }

        AIRY_LOG_INFO("gateway PEP: epoch watch subscribed via %s", a->path);

        char buf[2048];
        size_t used = 0;
        int idle = 0;
        for (;;) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
            int pr = poll(&pfd, 1, 5000);
            if (pr < 0)
                break;
            if (pr == 0) {
                /* 5s 无事件：计数保活，超时重连清除半死连接 */
                if (++idle >= 12)
                    break;
                continue;
            }
            if (!(pfd.revents & POLLIN))
                continue;
            ssize_t n = recv(fd, buf + used, sizeof(buf) - used - 1, 0);
            if (n <= 0)
                break;
            used += (size_t)n;
            buf[used] = '\0';
            idle = 0;

            /* 逐帧消费（SSE 帧分隔：\n\n；取最后 "data:" 行） */
            size_t start = 0;
            for (;;) {
                char *end = strstr(buf + start, "\n\n");
                if (!end)
                    break;
                const char *dl = strstr(buf + start, "data:");
                if (dl && dl < end) {
                    const char *ds = dl + strlen("data:");
                    while (*ds == ' ')
                        ds++;
                    size_t dl_len = (size_t)(end - ds);
                    if (dl_len >= sizeof(buf))
                        dl_len = sizeof(buf) - 1;
                    char line[2048];
                    __builtin_memcpy(line, ds, dl_len);
                    line[dl_len] = '\0';
                    uint64_t ep = gw_pep_epoch_parse(line);
                    if (ep)
                        adopt_epoch(ep);
                }
                start = (size_t)((end + 2) - buf);
            }
            if (start > 0) {
                __builtin_memmove(buf, buf + start, used - start);
                used -= start;
                buf[used] = '\0';
            }
        }
        close(fd);
        AIRY_LOG_WARN("gateway PEP: epoch watch disconnected, reconnecting");
        sleep(2);
    }
    return NULL;
}
#endif

/* 启动 epoch 订阅观察线程（进程级单次；调用点：gateway business ctx 初始化后）。
 * fail-open：notify_d 未上线/订阅失败仅重连重试，不影响懒对齐路径。 */
void gw_pep_epoch_observe(const gateway_business_ctx_t *ctx)
{
    if (!ctx || !ctx->notify_sock_path[0])
        return;
#ifndef _WIN32
    static volatile int s_started;
    if (s_started)
        return;
    epoch_watch_arg_t *a = AIRY_CALLOC(1, sizeof(*a));
    if (!a)
        return;
    AIRY_STRNCPY_TERM(a->path, ctx->notify_sock_path, sizeof(a->path));
    pthread_t th;
    if (pthread_create(&th, NULL, epoch_watch_main, a) != 0) {
        AIRY_FREE(a);
        return;
    }
    s_started = 1;
    pthread_detach(th);
    AIRY_LOG_INFO("gateway PEP: epoch observer started (notify=%s)", a->path);
#endif
}
