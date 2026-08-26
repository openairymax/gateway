// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file syscall_router.c
 * @brief Syscall router implementation.
 *
 * Routes JSON-RPC requests to syscalls uniformly.
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

/**
 * @brief 统一经 syscall 派发（架构约束 2026-08-25 "必须走 syscall"）。
 *
 * 调用 airy_sys_svc_call()（SYS_SVC_CALL=24）并经 gateway 注入的派发钩子
 * 路由到微核心服务 daemon，随后解包 JSON-RPC result 字段：
 *   - daemon_rpc_call（旧实现）返回解包后的 result JSON；
 *   - gw_svc_call（syscall 钩子内部传输）返回完整 JSON-RPC 响应。
 * 本函数保持 daemon_rpc_call 的返回语义（解包 result），使各调用点零改动。
 *
 * @param ns          命名空间（"sched"/"mem"/"agent"）
 * @param method      daemon 方法名
 * @param params_json 参数 JSON（可 NULL）
 * @param timeout_ms  超时毫秒
 * @param out_result  [out] 解包后的 result JSON 字符串（OWNER，AIRY_FREE）
 * @return AIRY_SUCCESS 成功；非 0 失败
 */
int syscall_svc_call_unwrap(const char *ns, const char *method, const char *params_json,
                            int timeout_ms, char **out_result)
{
    if (!ns || !method || !out_result)
        return AIRY_ERR_INVALID_PARAM;
    *out_result = NULL;

    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call(ns, method, params_json, (uint32_t)timeout_ms, &resp);
    if (rc != AIRY_SUCCESS || !resp) {
        AIRY_FREE(resp);
        return rc != AIRY_SUCCESS ? (int)rc : AIRY_ERR_GENERIC_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return AIRY_ERR_PARSE_ERROR;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    cJSON *result = err ? NULL : cJSON_GetObjectItem(root, "result");
    if (err || !result) {
        cJSON_Delete(root);
        return AIRY_ERR_GENERIC_FAIL;
    }
    char *out = cJSON_PrintUnformatted(result);
    cJSON_Delete(root);
    if (!out)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_result = out;
    return AIRY_SUCCESS;
}

/**
  * @brief Route a system call request
 */
char *gateway_syscall_route(const char *method, cJSON *params, cJSON *request_id)
{
    if (!method || strlen(method) == 0) {
        return jsonrpc_create_error_response(request_id, -32600, "Invalid Request", NULL);
    }

    /* Prefix match must use the exact literal length (sizeof-1). A longer
     * count makes strncmp read past the literal's NUL and compare against the
     * method's next char, so every airy_sys_* call fell through to
     * "Method not found" (root cause of syscalls never reaching the domains). */
    if (strncmp(method, "airy_sys_task_", sizeof("airy_sys_task_") - 1) == 0) {
        return route_task_methods(method, params, request_id);
    } else if (strncmp(method, "airy_sys_memory_", sizeof("airy_sys_memory_") - 1) == 0) {
        return route_memory_methods(method, params, request_id);
    } else if (strncmp(method, "airy_sys_session_", sizeof("airy_sys_session_") - 1) == 0) {
        return route_session_methods(method, params, request_id);
    } else if (strncmp(method, "airy_sys_telemetry_", sizeof("airy_sys_telemetry_") - 1) == 0) {
        return route_telemetry_methods(method, params, request_id);
    } else if (strncmp(method, "airy_sys_agent_", sizeof("airy_sys_agent_") - 1) == 0) {
        return route_agent_methods(method, params, request_id);
    }

    return jsonrpc_create_error_response(request_id, -32601, "Method not found", NULL);
}
