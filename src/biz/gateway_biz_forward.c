// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_forward.c
 * @brief Gateway namespace forwarding: L2 protocol client.
 *
 * Namespace forwarding handlers (<daemon>.<method>). The transport behind
 * gw_svc_call moved to the unified southbound A-IPC client face
 * (gateway_aipc_client.c) in 0.1.16 B3; this file keeps the legacy entry as
 * a thin wrapper plus the forwarding/ACL logic.
 *
 * 0.1.6 P1-4 收敛：外部可调用方法的枚举/白名单统一由能力注册表
 * （gateway_cap_registry.h，cap_key 单一权威源）承载，本文件不再维护
 * 任何方法清单；未登记能力在 gateway_business_handler.c 主派发处
 * fail-closed 拒绝（-32601），防止任意方法透传。
 *
 * Split from gateway_business_handler.c (single responsibility: namespace
 * forwarding).
 */

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"
#include "daemon_security.h"
#include "gateway_aipc_client.h" /* 0.1.16 B3: southbound face owns the transport */

#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *jsonrpc_error(int code, const char *msg, const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp)
        return NULL;
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg ? msg : "Unknown error");
    cJSON_AddItemToObject(resp, "error", err);

    if (id && !cJSON_IsNull(id)) {
        if (cJSON_IsString(id)) {
            cJSON_AddStringToObject(resp, "id", id->valuestring);
        } else if (cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(resp, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(resp, "id");
        }
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return out;
}

/**
 * @brief Generic daemon internal service call (legacy entry, 0.1.16 B3)
 *
 * Thin wrapper kept for zero call-site churn: the transport (L2-first per
 * blueprint 8.3.3, socket/TCP fallback) now lives in the unified southbound
 * A-IPC client face (gateway_aipc_client.c). See gw_aipc_call().
 *
 * @param sock_path   Target daemon socket path
 * @param method      Internal service method (e.g. "spawn"/"invoke"/"write")
 * @param params_json Method params JSON string (NULL/empty -> "{}")
 * @param timeout_ms  Receive timeout (ms)
 * @return Response JSON string (AIRY_MALLOC, caller AIRY_FREE), or NULL on failure
 */
char *gw_svc_call(const char *sock_path, const char *method, const char *params_json,
                  int timeout_ms)
{
    return gw_aipc_call(sock_path, method, params_json, timeout_ms);
}

/**
 * @brief ACL check for tool execution from external protocols
 *
 * M2-S5（0.1.9 §3.2 PEP）：经 gateway PEP 裁定缓存判定——命中缓存
 * 零 RPC，miss 时向 PDP（cupolas_d）请求裁定并以响应 epoch 对齐失效；
 * PDP 不可达降级本地 ACL（daemon_check_tool_permission，fail-closed）。
 *
 * @param ctx Gateway business ctx（含 cupolas_d socket 端点）
 * @param tool_name Tool name
 * @return 0 allowed, non-zero denied
 */
int gw_acl_check_tool(const gateway_business_ctx_t *ctx, const char *tool_name)
{
    if (!tool_name)
        return -1;
    int rc = gw_pep_check(ctx, GW_EXTERNAL_AGENT_ID, tool_name, "execute");
    if (rc != 0) {
        AIRY_LOG_WARN("gateway ACL DENY: agent=%s tool=%s (fail-closed)", GW_EXTERNAL_AGENT_ID,
                 tool_name);
        return -1;
    }
    return 0;
}

/**
 * @brief Namespace method forwarding: gateway JSON-RPC <ns>.<method> ->
 *        daemon <method>
 *
 * Same pass-through mode as handle_mem_call: params/response are forwarded
 * as-is, the response id is rewritten to the request id.
 *
 * 0.1.6 P1-4：方法存在性校验已由主派发经能力注册表（gw_cap_find）完成。
 * 0.1.9 M4：wire 方法名取自注册表（rule->method），不再按目标命名空间截取
 * 请求串——plugin.* 转发 tool 命名空间时前缀与目标不一致（plugin_* 方法）。
 *
 * @param rule Forwarding rule (ns/timeout/method，由能力注册表派生)
 * @return Complete JSON-RPC response string from the target daemon
 *         (AIRY_MALLOC), or an error response on failure
 */
char *handle_ns_forward(cJSON *root, const gw_ns_forward_rule_t *rule)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (!rule || !rule->ns || !rule->method || !*rule->method)
        return jsonrpc_error(-32601, "Method not found", id);

    cJSON *params = cJSON_GetObjectItem(root, "params");
    char *params_str = params ? cJSON_PrintUnformatted(params) : AIRY_STRDUP("{}");
    if (!params_str)
        return jsonrpc_error(-32603, "Out of memory", id);

    /* 架构约束 2026-08-25 "必须走 syscall": 命名空间转发统一经 SYS_SVC_CALL
     * 派发（钩子按命名空间路由到对应 daemon 端点，见 gateway_biz_svcdispatch.c）。 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call(rule->ns, rule->method, params_str,
                                      (uint32_t)rule->timeout_ms, &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp)
        return jsonrpc_error(-32603, "Service unreachable", id);

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot)
        return jsonrpc_error(-32603, "Service returned invalid response", id);

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *svc_id = cJSON_GetObjectItem(rroot, "id");
    if (svc_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/**
 * @brief mem.* forwarding: gateway JSON-RPC -> mem_d (params/response pass-through)
 *
 * 0.1.6 P1-4：mem.* 方法枚举由能力注册表（GW_CAP_KIND_MEM）承载，主派发
 * 已保证方法已登记；本函数直接转发内层方法名（<ns>.<method> 的 method）。
 *
 * Env-gated by AIRY_GATEWAY_MEM_PUBLIC (default true: internal memory service
 * traffic passes; false disables external mem access without affecting the TUI
 * local JSONL).
 */
char *handle_mem_call(cJSON *root)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    const char *method_str = cJSON_IsString(method) ? method->valuestring : NULL;
    cJSON *params = cJSON_GetObjectItem(root, "params");

    const char *mem_method = NULL;
    if (method_str && strncmp(method_str, "mem.", 4) == 0)
        mem_method = method_str + 4;
    if (!mem_method || !*mem_method)
        return jsonrpc_error(-32601, "Method not found", id);

    const char *pub = getenv("AIRY_GATEWAY_MEM_PUBLIC");
    if (pub && (strcmp(pub, "false") == 0 || strcmp(pub, "0") == 0)) {
        return jsonrpc_error(-32001, "Memory service access disabled", id);
    }

    char *params_str = NULL;
    if (params) {
        params_str = cJSON_PrintUnformatted(params);
    } else {
        params_str = AIRY_STRDUP("{}");
    }
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    /* 架构约束 2026-08-25 "必须走 syscall": mem.* 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("mem", mem_method, params_str, GW_TOOL_TIMEOUT_MS, &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        return jsonrpc_error(-32603, "Memory service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Memory service returned invalid response", id);
    }
    /* JSON-RPC 2.0 compliance: the response id must match the request id.
     * mem_d echoes the internal id=1 used by gw_svc_call; without rewriting,
     * concurrent requests cannot be correlated to their originals (client id
     * validation would fail). */
    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *mem_id = cJSON_GetObjectItem(rroot, "id");
    if (mem_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/**
 * @brief llm.list_models forwarding: gateway JSON-RPC -> llm_d list_models
 *
 * Returns all models from the llm_d provider registry plus
 * default_model/default_provider, for CLI/TUI model configuration (read-only,
 * no params, no API key needed). The response id is rewritten to the request
 * id (same concurrency compliance as handle_mem_call).
 */
char *handle_llm_list_models(cJSON *root, const gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */

    /* 架构约束 2026-08-25 "必须走 syscall": llm.list_models 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("llm", "list_models", "{}", GW_LLM_DEFAULT_TIMEOUT_MS,
                                      &resp);
    if (rc != AIRY_SUCCESS || !resp) {
        return jsonrpc_error(-32603, "LLM service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "LLM service returned invalid response", id);
    }

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *llm_id = cJSON_GetObjectItem(rroot, "id");
    if (llm_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/**
 * @brief tool.pending / tool.approve forwarding: gateway JSON-RPC -> tool_d
 *
 * P0 interactive permission approval (Claude Code style permission prompt):
 * external tool.pending -> tool_d "pending"; external tool.approve ->
 * tool_d "approve". params/response pass through, the response id is rewritten
 * to the request id (same concurrency compliance as handle_mem_call).
 */
char *handle_tool_approval_call(cJSON *root, const gateway_business_ctx_t *ctx,
                                const char *tool_method)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */

    if (strcmp(tool_method, "approve") == 0) {
        const cJSON *req_id = params ? cJSON_GetObjectItem(params, "request_id") : NULL;
        const cJSON *decision = params ? cJSON_GetObjectItem(params, "decision") : NULL;
        if (!cJSON_IsString(req_id) || !req_id->valuestring || !req_id->valuestring[0] ||
            !cJSON_IsString(decision) || !decision->valuestring || !decision->valuestring[0]) {
            return jsonrpc_error(-32602, "Invalid params: request_id and decision required", id);
        }
    }

    char *params_str = NULL;
    if (params) {
        params_str = cJSON_PrintUnformatted(params);
    } else {
        params_str = AIRY_STRDUP("{}");
    }
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    /* 架构约束 2026-08-25 "必须走 syscall": tool.pending/approve 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("tool", tool_method, params_str, GW_TOOL_TIMEOUT_MS, &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        return jsonrpc_error(-32603, "Tool service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Tool service returned invalid response", id);
    }

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *tool_id = cJSON_GetObjectItem(rroot, "id");
    if (tool_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}
