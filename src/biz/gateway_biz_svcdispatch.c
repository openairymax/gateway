// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_svcdispatch.c
 * @brief 微核心服务统一派发钩子注入（架构约束 2026-08-25 "必须走 syscall"）。
 *
 * gateway 对微核心服务（llm/think/sched/agent/tool/mem/... daemon）的派发
 * 统一经 airy_sys_svc_call()（SYS_SVC_CALL 系统调用）完成，禁止绕过 syscall
 * 直连 daemon socket。宿主（gateway_d）在 gateway_business_ctx_create 时注入
 * 本钩子：
 *
 *   airy_sys_svc_call(ns, method, params, timeout) -> g_svc_dispatch 钩子
 *     -> 经命名空间策略表（gateway/.manifest 构建期生成）解析端点
 *     -> gw_svc_call()（L2 socket 客户端）传输
 *     -> 不可达时按 V13.2 单向请求 supervisor 激活（限频 fire-and-forget）
 *
 * syscall 层（atoms/syscall/src/svc/svc_dispatch.c）保持与 daemon 层解耦
 * （IRON-6 跨层耦合禁令），本文件是 gateway 侧唯一的 ns -> 端点映射点。
 */

#include "gateway_biz_internal.h"

#include "gateway_policy.h"

#include "syscalls.h"

#include "logging.h"

#include "airy_time.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* 当前 gateway 业务上下文（钩子回调无 ctx 参数，用静态指针承载；
 * 单 gateway 进程内仅一个 ctx 实例，生命周期由 ctx_create/destroy 管理）。 */
static gateway_business_ctx_t *g_svc_ctx = NULL;

/* 端点绑定键 -> ctx 内 sock 路径字段偏移（生成表见 gateway_sock_gen.inc）。 */
typedef struct {
    const char *key;
    size_t off;
} gw_sock_bind_t;

#define GW_SOCK_FIELD(f) offsetof(gateway_business_ctx_t, f##_sock_path)

#include "gateway_sock_gen.inc"

static int gw_sock_off(const char *key)
{
    if (!key)
        return -1;
    for (size_t i = 0; i < GW_SOCKS_N; ++i) {
        if (strcmp(GW_SOCKS[i].key, key) == 0)
            return (int)GW_SOCKS[i].off;
    }
    return -1;
}

/* V13.2 按需激活限频：per-ns 单向通知最小间隔，保护 supervisor 串行控制口
 * （supervisor 侧 activate 幂等且不绕过退避；首写竞态最多多发一次，无害）。 */
#define GW_ACT_MIN_INTERVAL_MS 1000
static atomic_uint64_t g_act_last_ms[GW_NS_POL_MAX];

static int gw_act_throttled(size_t idx)
{
    uint64_t now = airy_time_monotonic_ms();
    uint64_t last = atomic_load(&g_act_last_ms[idx]);
    if (now < last + (uint64_t)GW_ACT_MIN_INTERVAL_MS)
        return 1;
    atomic_store(&g_act_last_ms[idx], now);
    return 0;
}

/**
 * @brief 命名空间策略 -> 宿主 wire 方法名（无前缀原样返回）。
 * @return wire 方法名（指向 buf 或入参 method，非 OWNER）；缓冲区溢出返回 NULL
 */
static const char *gw_wire(const gw_ns_pol_t *pol, const char *method,
                           char *buf, size_t buf_sz)
{
    int n;

    if (!pol->prefix)
        return method;
    if (pol->l2_pass && gw_l2_method(method))
        return method;
    n = snprintf(buf, buf_sz, "%s_%s", pol->prefix, method);
    if (n < 0 || (size_t)n >= buf_sz)
        return NULL;
    return buf;
}

/**
 * @brief 微核心服务派发钩子（airy_svc_dispatch_fn 实现）。
 * @return 0 成功（*out_result 为响应 JSON，调用方 AIRY_FREE）；
 *         非 0 失败（端点未知 / 服务不可达 / 参数非法）。
 */
static int gw_sys_svc_dispatch(const char *ns, const char *method, const char *params_json,
                               uint32_t timeout_ms, char **out_result)
{
    if (!ns || !method || !out_result) {
        if (out_result)
            *out_result = NULL;
        return -1;
    }
    *out_result = NULL;
    if (!g_svc_ctx)
        return -1;

    const gw_ns_pol_t *pol = gw_ns_pol(ns);
    if (!pol || !pol->route) {
        AIRY_LOG_WARN("gateway svc_dispatch: unknown namespace '%s' (method=%s)", ns, method);
        return -1;
    }
    int off = gw_sock_off(pol->sock);
    if (off < 0) {
        AIRY_LOG_ERROR("gateway svc_dispatch: ns=%s binds unknown endpoint '%s'",
                       pol->ns, pol->sock ? pol->sock : "(null)");
        return -1;
    }
    const char *sock = (const char *)(const void *)g_svc_ctx + off;

    /* 整编 ns（plugin / info / observe / policy）→ 宿主 wire 方法名前缀
     * 转换（"load" → "plugin_load"）。l2_pass 命名空间对三件套透传宿主
     * 自身语义。 */
    char wire_buf[64];
    const char *wire_method = gw_wire(pol, method, wire_buf, sizeof(wire_buf));
    if (!wire_method) {
        AIRY_LOG_WARN("gateway svc_dispatch: wire method too long ns=%s method=%s",
                      pol->ns, method);
        return -1;
    }

    char *resp = gw_svc_call(sock, wire_method, params_json, (int)timeout_ms);
    if (!resp) {
        AIRY_LOG_WARN("gateway svc_dispatch: service unreachable ns=%s method=%s sock=%s",
                      pol->ns, wire_method, sock);
        /* V13.2 按需激活：不可达即单向请求 supervisor 拉起（限频 +
         * fire-and-forget，不改写客户端可见错误；重试责任在客户端）。 */
        int slot = gw_ns_slot(pol);
        if (slot >= 0 && !gw_act_throttled((size_t)slot))
            gw_sup_notify_activate(pol->daemon);
        return -1;
    }
    *out_result = resp;
    return 0;
}

/**
 * @brief 注入 syscall 微核心服务派发钩子（幂等；ctx_create 时调用）。
 * @param ctx 网关业务上下文（端点已解析）
 * @return 0 成功
 */
int gw_sys_svc_dispatch_init(gateway_business_ctx_t *ctx)
{
    if (!ctx)
        return -1;
    g_svc_ctx = ctx;
    airy_sys_set_svc_dispatch(gw_sys_svc_dispatch);
    AIRY_LOG_INFO("gateway: svc dispatch hook injected (all daemon calls via SYS_SVC_CALL)");
    return 0;
}

/**
 * @brief 清除派发钩子（ctx_destroy 时调用）。
 */
void gw_sys_svc_dispatch_cleanup(void)
{
    airy_sys_set_svc_dispatch(NULL);
    g_svc_ctx = NULL;
}
