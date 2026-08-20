// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_telemetry.c
 * @brief Syscall router observability domain (airy_sys_telemetry_* implementation and routing).
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

/**
  * @brief Route observability syscalls
 */
char *route_telemetry_methods(const char *method, cJSON *params, cJSON *request_id)
{
    cJSON *result = NULL;
    airy_err_t err = AIRY_SUCCESS;

    if (strcmp(method, "airy_sys_telemetry_metrics") == 0) {
        char *out_metrics = NULL;
        err = airy_sys_telemetry_metrics(&out_metrics);

        if (err == AIRY_SUCCESS && out_metrics) {
            result = cJSON_Parse(out_metrics);
            AIRY_FREE(out_metrics);
        }
    } else if (strcmp(method, "airy_sys_telemetry_traces") == 0) {
        cJSON *trace_id = cJSON_GetObjectItem(params, "trace_id");
        const char *tid = (trace_id && cJSON_IsString(trace_id)) ? trace_id->valuestring : NULL;
        char *out_traces = NULL;
        err = airy_sys_telemetry_traces(tid, &out_traces);

        if (err == AIRY_SUCCESS && out_traces) {
            result = cJSON_Parse(out_traces);
            AIRY_FREE(out_traces);
        }
    }

    if (err != AIRY_SUCCESS) {
        cJSON_Delete(result);
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "System call failed: %d", err);
        return jsonrpc_create_error_response(request_id, -32000, err_msg, NULL);
    }

    return jsonrpc_create_success_response(request_id, result);
}

/* Telemetry */
airy_err_t airy_sys_telemetry_metrics(char **out_metrics)
{
    if (!out_metrics)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *metrics = cJSON_CreateObject();
    if (!metrics)
        return AIRY_ERR_OUT_OF_MEMORY;

    /* Phase 3: task/agent counters are owned by sched_d; forward get_stats when
      * the daemon is reachable, degrade to 0 otherwise (same as mem_d). */
    uint64_t total_tasks = 0;
    uint64_t agent_count = 0;
    {
        cJSON *params = cJSON_CreateObject();
        char *params_str = cJSON_PrintUnformatted(params);
        cJSON_Delete(params);
        if (params_str) {
            char *result_str = NULL;
            int rc = daemon_rpc_call(AIRY_SCHED_D_SOCKET, "get_stats", params_str, &result_str,
                                     AIRY_DAEMON_RPC_TIMEOUT_MS);
            AIRY_FREE(params_str);
            if (rc == AIRY_SUCCESS && result_str) {
                cJSON *root = cJSON_Parse(result_str);
                AIRY_FREE(result_str);
                if (root) {
                    cJSON *tt = cJSON_GetObjectItem(root, "total_tasks");
                    cJSON *ac = cJSON_GetObjectItem(root, "agent_count");
                    if (cJSON_IsNumber(tt))
                        total_tasks = (uint64_t)tt->valuedouble;
                    if (cJSON_IsNumber(ac))
                        agent_count = (uint64_t)ac->valuedouble;
                    cJSON_Delete(root);
                }
            } else {
                AIRY_FREE(result_str);
            }
        }
    }

    cJSON *runtime = cJSON_CreateObject();
    cJSON_AddNumberToObject(runtime, "total_tasks", (double)total_tasks);
    cJSON_AddNumberToObject(runtime, "active_tasks", 0.0);
    cJSON_AddNumberToObject(runtime, "memory_records", (double)g_runtime.record_count);
    cJSON_AddNumberToObject(runtime, "active_sessions", (double)g_runtime.session_count);
    cJSON_AddNumberToObject(runtime, "registered_agents", (double)agent_count);
    cJSON_AddNumberToObject(runtime, "memory_writes", (double)g_runtime.total_memory_writes);
    cJSON_AddItemToObject(metrics, "runtime", runtime);

    cJSON_AddStringToObject(metrics, "status", "operational");
    cJSON_AddNumberToObject(metrics, "uptime_seconds", (double)time(NULL));

    *out_metrics = cJSON_PrintUnformatted(metrics);
    cJSON_Delete(metrics);
    return AIRY_OK;
}

airy_err_t airy_sys_telemetry_traces(const char *trace_id, char **out_traces)
{
    if (!out_traces)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *traces = cJSON_CreateArray();
    if (trace_id && strlen(trace_id) > 0) {
        cJSON *trace = cJSON_CreateObject();
        cJSON_AddStringToObject(trace, "trace_id", trace_id);
        cJSON_AddStringToObject(trace, "service", "syscall_router");
        cJSON_AddStringToObject(trace, "status", "completed");
        cJSON_AddNumberToObject(trace, "duration_ms", 1.5);
        cJSON_AddItemToArray(traces, trace);
    }
    /* tracing array finalized */

    *out_traces = cJSON_PrintUnformatted(traces);
    cJSON_Delete(traces);
    return AIRY_OK;
}
