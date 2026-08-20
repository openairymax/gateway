// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_task.c
 * @brief Syscall router task domain (airy_sys_task_* IPC forwarding and routing).
 *
 * Phase 3 (executor consolidation): task execution is owned by the sched_d
 * daemon (schedule_task / get_task / cancel_task). The gateway keeps the
 * airy_sys_task_* signatures and ABI and only forwards JSON-RPC requests over
 * the daemon socket, mirroring the mem_d/agent_d forwarding pattern used by
 * the memory/agent domains.
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

#include "daemon_rpc_client.h"

/* TASK_STATUS_* macros are shared from commons/utils/types/include/types.h.
 * sched_d reports "completed" which maps to TASK_STATUS_SUCCEEDED. */

#define TASK_WAIT_POLL_INTERVAL_MS 100
#define TASK_DEFAULT_TIMEOUT_MS 30000

/**
  * @brief Route task-management syscalls
 */
char *route_task_methods(const char *method, cJSON *params, cJSON *request_id)
{
    cJSON *result = NULL;
    airy_err_t err = AIRY_SUCCESS;

    if (strcmp(method, "airy_sys_task_submit") == 0) {
        cJSON *input = cJSON_GetObjectItem(params, "input");
        cJSON *timeout = cJSON_GetObjectItem(params, "timeout_ms");

        if (!input || !cJSON_IsString(input)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: input required", NULL);
        }

        char *out_result = NULL;
        uint32_t timeout_ms = timeout ? (uint32_t)timeout->valueint : 0;
        err = airy_sys_task_submit(input->valuestring, strlen(input->valuestring), timeout_ms,
                                   &out_result);

        if (err == AIRY_SUCCESS && out_result) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "result", out_result);
            AIRY_FREE(out_result);
        }
    } else if (strcmp(method, "airy_sys_task_query") == 0) {
        cJSON *task_id = cJSON_GetObjectItem(params, "task_id");

        if (!task_id || !cJSON_IsString(task_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: task_id required", NULL);
        }

        int status = 0;
        err = airy_sys_task_query(task_id->valuestring, &status);

        if (err == AIRY_SUCCESS) {
            result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "status", status);
        }
    } else if (strcmp(method, "airy_sys_task_wait") == 0) {
        cJSON *task_id = cJSON_GetObjectItem(params, "task_id");
        cJSON *timeout = cJSON_GetObjectItem(params, "timeout_ms");

        if (!task_id || !cJSON_IsString(task_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: task_id required", NULL);
        }

        char *out_result = NULL;
        uint32_t timeout_ms = timeout ? (uint32_t)timeout->valueint : 0;
        err = airy_sys_task_wait(task_id->valuestring, timeout_ms, &out_result);

        if (err == AIRY_SUCCESS && out_result) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "result", out_result);
            AIRY_FREE(out_result);
        }
    } else if (strcmp(method, "airy_sys_task_cancel") == 0) {
        cJSON *task_id = cJSON_GetObjectItem(params, "task_id");

        if (!task_id || !cJSON_IsString(task_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: task_id required", NULL);
        }

        err = airy_sys_task_cancel(task_id->valuestring);
        if (err == AIRY_SUCCESS) {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "cancelled", true);
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

/**
  * @brief Map a sched_d status string to the task ABI status integer.
  */
static int task_status_from_string(const char *s)
{
    if (!s)
        return TASK_STATUS_PENDING;
    if (strcmp(s, "pending") == 0)
        return TASK_STATUS_PENDING;
    if (strcmp(s, "running") == 0)
        return TASK_STATUS_RUNNING;
    if (strcmp(s, "completed") == 0)
        return TASK_STATUS_SUCCEEDED;
    if (strcmp(s, "failed") == 0)
        return TASK_STATUS_FAILED;
    if (strcmp(s, "canceled") == 0)
        return TASK_STATUS_CANCELLED;
    return TASK_STATUS_PENDING;
}

/**
  * @brief Query sched_d for the current task status.
  * @return AIRY_OK when the daemon answered, the task ABI status via *status.
  */
static int task_daemon_get_status(const char *task_id, int *status)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "task_id", task_id);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_SCHED_D_SOCKET, "get_task", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_task_query: malformed result JSON");
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON *st = cJSON_GetObjectItem(result, "status");
    const char *sname = (st && cJSON_IsString(st)) ? st->valuestring : "unknown";
    *status = task_status_from_string(sname);
    cJSON_Delete(result);
    return AIRY_OK;
}

airy_err_t airy_sys_task_submit(const char *input, size_t len, uint32_t timeout_ms,
                                char **out_result)
{
    if (!input || !out_result)
        return AIRY_ERR_INVALID_PARAM;
    (void)len; /* len is a C-string convention artifact; input is NUL-terminated */
    *out_result = NULL;

    cJSON *params = cJSON_CreateObject();
    cJSON *task = cJSON_CreateObject();
    cJSON_AddStringToObject(task, "task_description", input);
    cJSON_AddNumberToObject(task, "priority", 0);
    cJSON_AddNumberToObject(task, "timeout_ms",
                            (double)(timeout_ms ? timeout_ms : TASK_DEFAULT_TIMEOUT_MS));
    cJSON_AddItemToObject(params, "task", task);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_SCHED_D_SOCKET, "schedule_task", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    *out_result = result_str;
    return AIRY_OK;
}

airy_err_t airy_sys_task_query(const char *task_id, int *status)
{
    if (!task_id || !status)
        return AIRY_ERR_INVALID_PARAM;

    int st = TASK_STATUS_PENDING;
    int rc = task_daemon_get_status(task_id, &st);
    if (rc != AIRY_SUCCESS)
        return rc;
    *status = st;
    return AIRY_OK;
}

airy_err_t airy_sys_task_wait(const char *task_id, uint32_t timeout_ms, char **out_result)
{
    if (!task_id || !out_result)
        return AIRY_ERR_INVALID_PARAM;
    *out_result = NULL;

    uint32_t budget_ms = timeout_ms ? timeout_ms : TASK_DEFAULT_TIMEOUT_MS;
    uint32_t waited_ms = 0;

    for (;;) {
        int status = TASK_STATUS_PENDING;
        int rc = task_daemon_get_status(task_id, &status);
        if (rc != AIRY_SUCCESS)
            return rc;

        if (status == TASK_STATUS_SUCCEEDED || status == TASK_STATUS_FAILED ||
            status == TASK_STATUS_CANCELLED) {
            /* Return the final daemon report (task_id/status/output/error). */
            cJSON *params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "task_id", task_id);
            char *params_str = cJSON_PrintUnformatted(params);
            cJSON_Delete(params);
            if (!params_str)
                return AIRY_ERR_OUT_OF_MEMORY;

            char *result_str = NULL;
            rc = daemon_rpc_call(AIRY_SCHED_D_SOCKET, "get_task", params_str, &result_str,
                                 AIRY_DAEMON_RPC_TIMEOUT_MS);
            AIRY_FREE(params_str);
            if (rc != AIRY_SUCCESS)
                return rc;
            *out_result = result_str;
            return AIRY_OK;
        }

        if (waited_ms >= budget_ms)
            return AIRY_ERR_TIMEOUT;

        uint32_t step = TASK_WAIT_POLL_INTERVAL_MS;
        if (step > budget_ms - waited_ms)
            step = budget_ms - waited_ms;
        airy_sleep_ms(step);
        waited_ms += step;
    }
}

airy_err_t airy_sys_task_cancel(const char *task_id)
{
    if (!task_id)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "task_id", task_id);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_SCHED_D_SOCKET, "cancel", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    AIRY_FREE(result_str);
    return rc;
}
