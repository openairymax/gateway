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
