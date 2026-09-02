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
 *     -> 按命名空间映射到 ctx 中已解析的 daemon 端点（环境变量覆盖优先）
 *     -> gw_svc_call()（L2 socket 客户端）传输
 *
 * syscall 层（atoms/syscall/src/svc/svc_dispatch.c）保持与 daemon 层解耦
 * （IRON-6 跨层耦合禁令），本文件是 gateway 侧唯一的 ns -> 端点映射点。
 */

#include "gateway_biz_internal.h"

#include "syscalls.h"

#include "logging.h"

#include <stdio.h>
#include <string.h>

/* 当前 gateway 业务上下文（钩子回调无 ctx 参数，用静态指针承载；
 * 单 gateway 进程内仅一个 ctx 实例，生命周期由 ctx_create/destroy 管理）。 */
static gateway_business_ctx_t *g_svc_ctx = NULL;

/**
 * @brief 命名空间 -> daemon 端点映射（端点已在 ctx 创建时解析，
 *        支持 AIRY_<NS>_SOCK 环境变量覆盖与 Windows TCP 回环约定）。
 * @param ns 命名空间（可带尾点，如 "llm." 与 "llm" 等价）
 * @return 端点字符串（ctx 内存储，非 OWNER）；未知命名空间返回 NULL
 */
static const char *gw_svc_sock_for_ns(const char *ns)
{
    if (!ns || !g_svc_ctx)
        return NULL;

    char buf[32];
    size_t n = strlen(ns);
    if (n == 0 || n >= sizeof(buf))
        return NULL;
    AIRY_MEMCPY(buf, ns, n);
    if (buf[n - 1] == '.')
        buf[--n] = '\0';
    else
        buf[n] = '\0';

    if (strcmp(buf, "llm") == 0)
        return g_svc_ctx->llm_sock_path;
    if (strcmp(buf, "tool") == 0)
        return g_svc_ctx->tool_sock_path;
    if (strcmp(buf, "agent") == 0)
        return g_svc_ctx->agent_sock_path;
    if (strcmp(buf, "mem") == 0)
        return g_svc_ctx->mem_sock_path;
    if (strcmp(buf, "sched") == 0)
        return g_svc_ctx->sched_sock_path;
    if (strcmp(buf, "think") == 0)
        return g_svc_ctx->think_sock_path;
    if (strcmp(buf, "a2a") == 0)
        return g_svc_ctx->a2a_sock_path;
    /* 0.1.9 M4：plugin_d → tool_d 整编——旧 plugin ns 解析到 tool.sock，
     * 方法名在 gw_wire_method 内加 "plugin_" 前缀（见下） */
    if (strcmp(buf, "plugin") == 0)
        return g_svc_ctx->tool_sock_path;
    /* 0.1.9 M4：info_d / observe_d → monit_d 整编——旧 ns 解析到 monit.sock，
     * 方法名在 gw_wire_method 内加 "info_" / "observe_" 前缀（见下） */
    if (strcmp(buf, "info") == 0)
        return g_svc_ctx->monit_sock_path;
    if (strcmp(buf, "notify") == 0)
        return g_svc_ctx->notify_sock_path;
    if (strcmp(buf, "observe") == 0)
        return g_svc_ctx->monit_sock_path;
    if (strcmp(buf, "market") == 0)
        return g_svc_ctx->market_sock_path;
    if (strcmp(buf, "hook") == 0)
        return g_svc_ctx->hook_sock_path;
    if (strcmp(buf, "monit") == 0)
        return g_svc_ctx->monit_sock_path;
    if (strcmp(buf, "channel") == 0)
        return g_svc_ctx->channel_sock_path;
    if (strcmp(buf, "cupolas") == 0)
        return g_svc_ctx->cupolas_sock_path;
    return NULL;
}

/* 0.1.9 M4：daemon 整编命名空间路由表（plugin→tool、info/observe→monit）。
 * 旧 ns 的 syscall 调用在 wire 方法名上加 "<legacy_ns>_" 前缀；l2_pass=1
 * 表示宿主未登记带前缀的 L2 变体（info / observe 整编情形），shutdown /
 * get_stats / health_check 三件套透传宿主自身语义；l2_pass=0 保持整编前
 * 既有全前缀行为（tool_d 已登记 plugin_get_stats 等方法）。 */
static const struct {
    const char *ns;
    int l2_pass;
} GW_LEGACY_NS[] = {
    {"plugin", 0},
    {"info", 1},
    {"observe", 1},
};

/* L2 标准方法集（core dispatcher 约定，各 daemon 一致）。 */
static const char *const GW_L2_METHODS[] = {"shutdown", "get_stats", "health_check"};

static int gw_is_l2_method(const char *method)
{
    for (size_t i = 0; i < sizeof(GW_L2_METHODS) / sizeof(GW_L2_METHODS[0]); i++) {
        if (strcmp(method, GW_L2_METHODS[i]) == 0)
            return 1;
    }
    return 0;
}

/**
 * @brief legacy ns -> 宿主 daemon 的 wire 方法名转换（非 legacy ns 原样返回）。
 * @return wire 方法名（指向 buf 或入参 method，非 OWNER）；缓冲区溢出返回 NULL
 */
static const char *gw_wire_method(const char *ns, const char *method, char *buf, size_t buf_sz)
{
    for (size_t i = 0; i < sizeof(GW_LEGACY_NS) / sizeof(GW_LEGACY_NS[0]); i++) {
        const char *legacy = GW_LEGACY_NS[i].ns;
        size_t l = strlen(legacy);
        if (strncmp(ns, legacy, l) != 0 || (ns[l] != '\0' && ns[l] != '.'))
            continue;
        if (GW_LEGACY_NS[i].l2_pass && gw_is_l2_method(method))
            return method;
        int n = snprintf(buf, buf_sz, "%s_%s", legacy, method);
        if (n < 0 || (size_t)n >= buf_sz)
            return NULL;
        return buf;
    }
    return method;
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

    const char *sock = gw_svc_sock_for_ns(ns);
    if (!sock) {
        AIRY_LOG_WARN("gateway svc_dispatch: unknown namespace '%s' (method=%s)", ns, method);
        return -1;
    }

    /* 0.1.9 M4：legacy ns（plugin / info / observe）→ 宿主 wire 方法名前缀转换
     * （"load" → "plugin_load"、"system" → "info_system"）。兼容直接以旧 ns
     * 发起 syscall 的客户端；宿主侧带前缀方法与原生方法同表登记，前缀即消歧。 */
    char wire_buf[64];
    const char *wire_method = gw_wire_method(ns, method, wire_buf, sizeof(wire_buf));
    if (!wire_method) {
        AIRY_LOG_WARN("gateway svc_dispatch: wire method too long ns=%s method=%s", ns, method);
        return -1;
    }

    char *resp = gw_svc_call(sock, wire_method, params_json, (int)timeout_ms);
    if (!resp) {
        AIRY_LOG_WARN("gateway svc_dispatch: service unreachable ns=%s method=%s sock=%s", ns,
                      wire_method, sock);
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
