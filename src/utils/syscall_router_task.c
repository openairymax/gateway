// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_task.c
 * @brief 系统调用路由器任务域（airy_sys_task_* 实现与路由分发）
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

/**
 * @brief 路由任务管理相关系统调用
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

airy_err_t airy_sys_task_submit(const char *input, size_t len, uint32_t timeout_ms,
                                char **out_result)
{
    if (!input || !out_result)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    if (g_runtime.task_count >= g_max_tasks) {
        RUNTIME_UNLOCK();
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* P0: 参数校验前置，避免副作用先于校验（原实现先 task_count++ 再校验
     * len，超限时留下已占用的空槽） */
    if (len > MAX_INPUT_SIZE) {
        RUNTIME_UNLOCK();
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    task_entry_t *task = &g_runtime.tasks[g_runtime.task_count];
    task->task_id = AIRY_STRDUP(generate_uuid());
    if (!task->task_id) {

        RUNTIME_UNLOCK();
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    task->input = AIRY_STRNDUP(input, len);
    if (!task->input) {
        AIRY_FREE(task->task_id);
        task->task_id = NULL;
        RUNTIME_UNLOCK();
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    task->input_len = len;
    task->status = 1;
    task->result = NULL;
    task->timeout_ms = timeout_ms ? timeout_ms : 30000;
    task->created_at = time(NULL);
    g_runtime.task_count++;
    ht_insert(&g_runtime.task_index, task->task_id, g_runtime.task_count - 1);
    g_runtime.total_tasks_submitted++;
    RUNTIME_UNLOCK();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "task_id", task->task_id);
    cJSON_AddNumberToObject(resp, "status", task->status);
    cJSON_AddStringToObject(resp, "message", "Task accepted and queued");
    *out_result = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    return AIRY_OK;
}

airy_err_t airy_sys_task_query(const char *task_id, int *status)
{
    if (!task_id || !status)
        return AIRY_ERR_INVALID_PARAM;
    RUNTIME_LOCK();
    ssize_t idx = ht_lookup(&g_runtime.task_index, task_id);
    if (idx >= 0 && (size_t)idx < g_runtime.task_count) {
        *status = g_runtime.tasks[idx].status;
        RUNTIME_UNLOCK();
        return AIRY_OK;
    }
    RUNTIME_UNLOCK();
    *status = -1;
    return AIRY_ERR_NOT_FOUND;
}

airy_err_t airy_sys_task_wait(const char *task_id, uint32_t timeout_ms, char **out_result)
{
    if (!task_id || !out_result)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    ssize_t idx = ht_lookup(&g_runtime.task_index, task_id);
    if (idx >= 0 && (size_t)idx < g_runtime.task_count) {
        g_runtime.tasks[idx].status = 2;
        g_runtime.tasks[idx].result = AIRY_STRDUP("{\"output\":\"processed\",\"exit_code\":0}");

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "task_id", task_id);
        cJSON_AddNumberToObject(resp, "status", 2);
        cJSON_AddStringToObject(resp, "result", g_runtime.tasks[idx].result);
        *out_result = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        RUNTIME_UNLOCK();
        return AIRY_OK;
    }
    RUNTIME_UNLOCK();
    *out_result = AIRY_STRDUP("{}");
    return AIRY_ERR_NOT_FOUND;
}

airy_err_t airy_sys_task_cancel(const char *task_id)
{
    if (!task_id)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    ssize_t idx = ht_lookup(&g_runtime.task_index, task_id);
    if (idx >= 0 && (size_t)idx < g_runtime.task_count) {
        g_runtime.tasks[idx].status = 4;
        RUNTIME_UNLOCK();
        return AIRY_OK;
    }
    RUNTIME_UNLOCK();
    return AIRY_ERR_NOT_FOUND;
}
