/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
/**
 * @file gateway_biz_internal.h
 * @brief Internal shared definitions of the gateway business handler
 *        (module-private, not for external export).
 *
 * Splits the 2608-line gateway_business_handler.c into four files by
 * single responsibility; this header carries the shared contract among
 * them:
 *   - gateway_biz_forward.c  namespace forwarding (L2 protocol client + whitelist)
 *   - gateway_biz_agent.c    agent.run orchestration (dual-think + tool loop + cancel)
 *   - gateway_biz_backend.c  MCP/OpenAI/A2A protocol backends
 *   - gateway_business_handler.c  main dispatch and ctx lifecycle
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H
#define AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <cjson/cJSON.h>

#include "gateway_business_handler.h"
#include "gateway_mcp_server.h"

#include "airy_memory.h"
#include "atomic_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_LLM_DEFAULT_MODEL "deepseek-v4-flash"
/* LLM full-response timeout 90s: long-thinking / multi-tool_call rounds can
 * exceed 30s; the old 30s value made the gateway hit recv timeout while the
 * LLM had not yet returned, breaking the tool chain. */
#define GW_LLM_DEFAULT_TIMEOUT_MS 90000
/* think.process timeout 120s: dual thinking (GCCP probe/confirm + GRAD
 * multi-round quadruple-check) involves several LLM calls, each measured at
 * 15-25s; 120s covers the worst case. */
#define GW_THINK_TIMEOUT_MS 120000
#define GW_LLM_MAX_RESP 1048576
#define GW_LLM_DEFAULT_TCP_PORT 8080
#define GW_EXTERNAL_AGENT_ID "external"

/* Tool execution timeout 90s: shell_run itself times out at 60s; the old 30s
 * value would hit recv timeout before the tool finished, making the gateway
 * wrongly report long commands as failed. */
#define GW_TOOL_TIMEOUT_MS 90000

/* @brief Gateway business context: resolved daemon socket paths + model config
 *
 * All daemon endpoints are resolved once at create time
 * (env override -> $AIRY_RUNTIME_DIR/<name>.sock -> <name>.sock). */
struct gateway_business_ctx_s {
    char llm_sock_path[256];
    char llm_tcp_addr[64];
    uint16_t llm_tcp_port;
    char tool_sock_path[256];
    char agent_sock_path[256];
    char mem_sock_path[256];
    char sched_sock_path[256];
    char think_sock_path[256];
    char a2a_sock_path[256];
    char plugin_sock_path[256];
    char info_sock_path[256];
    char notify_sock_path[256];
    char observe_sock_path[256];
    char market_sock_path[256];
    char hook_sock_path[256];
    char monit_sock_path[256];
    char channel_sock_path[256];
    char cupolas_sock_path[256];
    char default_model[128];

    gateway_shutdown_fn_t on_shutdown;
    void *shutdown_user_data;
};

/* @brief Namespace forwarding rule: <ns>.<method> -> target daemon <method>
 *
 * 0.1.6 P1-4 收敛：方法白名单不再由 rule 携带——能力存在性校验统一走
 * 能力注册表（gateway_cap_registry.h，cap_key 单一权威源）。rule 仅承载
 * 命名空间（裸名，无尾点）与转发超时；daemon 端点由 svc dispatch 钩子
 * 按命名空间解析（gw_svc_sock_for_ns），无需在此维护。 */
typedef struct {
    const char *ns;
    int timeout_ms;
} gw_ns_forward_rule_t;

/* ---- gateway_biz_tools.c（内置 MCP 工具注册 SSoT, 2026-08-30 S-6）----
 * 内置工具 JSON schema 唯一权威源；返回注册失败数（0 = 全成功）。 */
int gw_biz_mcp_register_tools(gw_mcp_server_t *mcp, void *user_data);

/* ---- gateway_biz_forward.c (L2 protocol client + namespace forwarding) ---- */
char *jsonrpc_error(int code, const char *msg, const cJSON *id);
char *gw_svc_call(const char *sock_path, const char *method, const char *params_json,
                  int timeout_ms);
int gw_acl_check_tool(const gateway_business_ctx_t *ctx, const char *tool_name);
char *handle_ns_forward(cJSON *root, const gw_ns_forward_rule_t *rule);
char *handle_mem_call(cJSON *root);
char *handle_llm_list_models(cJSON *root, const gateway_business_ctx_t *ctx);
char *handle_tool_approval_call(cJSON *root, const gateway_business_ctx_t *ctx,
                                const char *tool_method);

/* ---- gateway_pep_cache.c (M2-S5 PEP 裁定缓存：epoch 失效键) ---- */
#include "gateway_pep_cache.h"

/* ---- gateway_biz_svcdispatch.c (微核心服务统一派发钩子, 2026-08-25) ---- */
int gw_sys_svc_dispatch_init(gateway_business_ctx_t *ctx);
void gw_sys_svc_dispatch_cleanup(void);

/* ---- gateway_biz_hall.c (hall.* — task board / event stream / chain) ---- */
char *handle_hall_call(cJSON *root, gateway_business_ctx_t *ctx);

/* ---- gateway_biz_agent.c (agent.run / agent.cancel 转发, M1-1a 引擎下沉) ----
 * agent.run 引擎（会话注册表/编排/工具循环）已迁 agent_d；gateway 仅转发。 */
char *handle_agent_run(cJSON *root, gateway_business_ctx_t *ctx);
char *handle_agent_cancel(cJSON *root, gateway_business_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H */
