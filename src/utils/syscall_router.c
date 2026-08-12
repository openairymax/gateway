// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * @file syscall_router.c
 * @brief 系统调用路由器实现
 *
 * 统一处理 JSON-RPC 请求到系统调用的路由。
 *
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

/**
 * @brief 路由系统调用请求
 */
char *gateway_syscall_route(const char *method, cJSON *params, cJSON *request_id)
{
    if (!method || strlen(method) == 0) {
        return jsonrpc_create_error_response(request_id, -32600, "Invalid Request", NULL);
    }

    if (strncmp(method, "airy_sys_task_", 18) == 0) {
        return route_task_methods(method, params, request_id);
    } else if (strncmp(method, "airy_sys_memory_", 20) == 0) {
        return route_memory_methods(method, params, request_id);
    } else if (strncmp(method, "airy_sys_session_", 20) == 0) {
        return route_session_methods(method, params, request_id);
    } else if (strncmp(method, "airy_sys_telemetry_", 22) == 0) {
        return route_telemetry_methods(method, params, request_id);
    } else if (strncmp(method, "airy_sys_agent_", 18) == 0) {
        return route_agent_methods(method, params, request_id);
    }

    return jsonrpc_create_error_response(request_id, -32601, "Method not found", NULL);
}
