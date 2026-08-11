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

#include "daemon_rpc_client.h"
#include "error.h"
#include "error.h"
#include "jsonrpc.h"
#include "logging.h"
#include "airy_memory.h"
#include "platform.h"
#include "string_compat.h"
#include "svc_logger.h"
#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase 3：执行体集中化重构 — daemon Unix socket 路径
 *
 * airy_sys_memory_* / airy_sys_agent_* 已迁移至 mem_d / agent_d 守护进程，
 * 本文件内仅保留 thin IPC client 转发逻辑。socket 路径与 daemon 端约定一致
 * （见 daemons/mem_d/src/main.c 与 daemons/agent_d/src/main.c）。 */
#ifndef AIRY_MEM_D_SOCKET
#define AIRY_MEM_D_SOCKET AIRY_RUNTIME_DIR "/mem.sock"
#endif
#ifndef AIRY_AGENT_D_SOCKET
#define AIRY_AGENT_D_SOCKET AIRY_RUNTIME_DIR "/agent.sock"
#endif
#define AIRY_DAEMON_RPC_TIMEOUT_MS 30000

#define RUNTIME_LOCK() airy_mtx_lock(&g_runtime.mutex)
#define RUNTIME_UNLOCK() airy_mtx_unlock(&g_runtime.mutex)

/**
 * @brief 路由任务管理相关系统调用
 */
static char *route_task_methods(const char *method, cJSON *params, cJSON *request_id)
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
 * @brief 路由记忆管理相关系统调用
 */
static char *route_memory_methods(const char *method, cJSON *params, cJSON *request_id)
{
    cJSON *result = NULL;
    airy_err_t err = AIRY_SUCCESS;

    if (strcmp(method, "airy_sys_memory_write") == 0) {
        cJSON *data = cJSON_GetObjectItem(params, "data");
        cJSON *metadata = cJSON_GetObjectItem(params, "metadata");

        if (!data || !cJSON_IsString(data)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: data required", NULL);
        }

        char *out_record_id = NULL;
        const char *meta_str = metadata ? cJSON_PrintUnformatted(metadata) : NULL;
        err = airy_sys_memory_write(data->valuestring, strlen(data->valuestring), meta_str,
                                    &out_record_id);

        if (meta_str)
            AIRY_FREE((void *)meta_str);

        if (err == AIRY_SUCCESS && out_record_id) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "record_id", out_record_id);
            AIRY_FREE(out_record_id);
        }
    } else if (strcmp(method, "airy_sys_memory_search") == 0) {
        cJSON *query = cJSON_GetObjectItem(params, "query");
        cJSON *limit = cJSON_GetObjectItem(params, "limit");

        if (!query || !cJSON_IsString(query)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: query required", NULL);
        }

        char **record_ids = NULL;
        float *scores = NULL;
        size_t count = 0;
        uint32_t lim = limit ? (uint32_t)limit->valueint : 10;

        err = airy_sys_memory_search(query->valuestring, lim, &record_ids, &scores, &count);

        if (err == AIRY_SUCCESS) {
            result = cJSON_CreateObject();
            cJSON *results = cJSON_CreateArray();
            for (size_t i = 0; i < count; i++) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "record_id", record_ids[i]);
                cJSON_AddNumberToObject(item, "score", scores[i]);
                cJSON_AddItemToArray(results, item);
                AIRY_FREE(record_ids[i]);
            }
            cJSON_AddItemToObject(result, "results", results);
            cJSON_AddNumberToObject(result, "total", count);
            AIRY_FREE(record_ids);
            AIRY_FREE(scores);
        }
    } else if (strcmp(method, "airy_sys_memory_get") == 0) {
        cJSON *record_id = cJSON_GetObjectItem(params, "record_id");

        if (!record_id || !cJSON_IsString(record_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: record_id required", NULL);
        }

        void *out_data = NULL;
        size_t out_len = 0;
        err = airy_sys_memory_get(record_id->valuestring, &out_data, &out_len);

        if (err == AIRY_SUCCESS && out_data) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", (char *)out_data);
            cJSON_AddNumberToObject(result, "length", out_len);
            AIRY_FREE(out_data);
        }
    } else if (strcmp(method, "airy_sys_memory_delete") == 0) {
        cJSON *record_id = cJSON_GetObjectItem(params, "record_id");

        if (!record_id || !cJSON_IsString(record_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: record_id required", NULL);
        }

        err = airy_sys_memory_delete(record_id->valuestring);
        if (err == AIRY_SUCCESS) {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "deleted", true);
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
 * @brief 路由会话管理相关系统调用
 */
static char *route_session_methods(const char *method, cJSON *params, cJSON *request_id)
{
    cJSON *result = NULL;
    airy_err_t err = AIRY_SUCCESS;

    if (strcmp(method, "airy_sys_session_create") == 0) {
        cJSON *metadata = cJSON_GetObjectItem(params, "metadata");
        char *out_session_id = NULL;
        const char *meta_str = metadata ? cJSON_PrintUnformatted(metadata) : NULL;

        err = airy_sys_session_create(meta_str, &out_session_id);

        if (meta_str)
            AIRY_FREE((void *)meta_str);

        if (err == AIRY_SUCCESS && out_session_id) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "session_id", out_session_id);
            AIRY_FREE(out_session_id);
        }
    } else if (strcmp(method, "airy_sys_session_get") == 0) {
        cJSON *session_id = cJSON_GetObjectItem(params, "session_id");

        if (!session_id || !cJSON_IsString(session_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: session_id required", NULL);
        }

        char *out_info = NULL;
        err = airy_sys_session_get(session_id->valuestring, &out_info);

        if (err == AIRY_SUCCESS && out_info) {
            result = cJSON_Parse(out_info);
            AIRY_FREE(out_info);
        }
    } else if (strcmp(method, "airy_sys_session_close") == 0) {
        cJSON *session_id = cJSON_GetObjectItem(params, "session_id");

        if (!session_id || !cJSON_IsString(session_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: session_id required", NULL);
        }

        err = airy_sys_session_close(session_id->valuestring);
        if (err == AIRY_SUCCESS) {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "closed", true);
        }
    } else if (strcmp(method, "airy_sys_session_list") == 0) {
        char **sessions = NULL;
        size_t session_count = 0;
        err = airy_sys_session_list(&sessions, &session_count);

        if (err == AIRY_SUCCESS && sessions) {
            result = cJSON_CreateArray();
            for (size_t i = 0; i < session_count && sessions[i]; i++) {
                cJSON_AddItemToArray(result, cJSON_CreateString(sessions[i]));
                AIRY_FREE(sessions[i]);
            }
            AIRY_FREE(sessions);
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
 * @brief 路由可观测性相关系统调用
 */
static char *route_telemetry_methods(const char *method, cJSON *params, cJSON *request_id)
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

/**
 * @brief 路由 Agent 管理相关系统调用
 */
static char *route_agent_methods(const char *method, cJSON *params, cJSON *request_id)
{
    cJSON *result = NULL;
    airy_err_t err = AIRY_SUCCESS;

    if (strcmp(method, "airy_sys_agent_spawn") == 0) {
        cJSON *spec = cJSON_GetObjectItem(params, "agent_spec");

        if (!spec || !cJSON_IsString(spec)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: agent_spec required", NULL);
        }

        char *out_agent_id = NULL;
        const char *spec_str = cJSON_PrintUnformatted(spec);
        err = airy_sys_agent_spawn(spec_str, &out_agent_id);

        if (spec_str)
            AIRY_FREE((void *)spec_str);

        if (err == AIRY_SUCCESS && out_agent_id) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "agent_id", out_agent_id);
            AIRY_FREE(out_agent_id);
        }
    } else if (strcmp(method, "airy_sys_agent_terminate") == 0) {
        cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");

        if (!agent_id || !cJSON_IsString(agent_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: agent_id required", NULL);
        }

        err = airy_sys_agent_terminate(agent_id->valuestring);
        if (err == AIRY_SUCCESS) {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "terminated", true);
        }
    } else if (strcmp(method, "airy_sys_agent_invoke") == 0) {
        cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");
        cJSON *input = cJSON_GetObjectItem(params, "input");

        if (!agent_id || !cJSON_IsString(agent_id)) {
            return jsonrpc_create_error_response(request_id, -32602,
                                                 "Invalid params: agent_id required", NULL);
        }

        const char *input_str = input && cJSON_IsString(input) ? input->valuestring : "";
        char *out_output = NULL;

        err =
            airy_sys_agent_invoke(agent_id->valuestring, input_str, strlen(input_str), &out_output);

        if (err == AIRY_SUCCESS && out_output) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "output", out_output);
            AIRY_FREE(out_output);
        }
    } else if (strcmp(method, "airy_sys_agent_list") == 0) {
        char **agent_ids = NULL;
        size_t count = 0;

        err = airy_sys_agent_list(&agent_ids, &count);

        if (err == AIRY_SUCCESS) {
            result = cJSON_CreateObject();
            cJSON *ids = cJSON_CreateArray();
            for (size_t i = 0; i < count; i++) {
                cJSON_AddItemToArray(ids, cJSON_CreateString(agent_ids[i]));
                AIRY_FREE(agent_ids[i]);
            }
            cJSON_AddItemToObject(result, "agent_ids", ids);
            cJSON_AddNumberToObject(result, "total", count);
            AIRY_FREE(agent_ids);
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

#include <time.h>

#define MAX_TASKS_DEFAULT 256
#define MAX_SESSIONS_DEFAULT 64
#include <airymax/sched.h>

#define MAX_INPUT_SIZE 4096

static size_t g_max_tasks = 0;
static size_t g_max_sessions = 0;

typedef struct {
    char *key;
    size_t index;
    bool occupied;
    bool deleted; /**< tombstone：删除标记。P0: 删除槽位不直接置空，
                       否则开放寻址探测链断裂，后续 ht_lookup 会漏查元素 */
} hash_entry_t;

typedef struct {
    hash_entry_t *entries;
    size_t capacity;
    size_t count;
} hash_table_t;

static unsigned long hash_fn(const char *str)
{
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        h = ((h << 5) + h) + c;
    return h;
}

static int ht_init(hash_table_t *ht, size_t capacity)
{
    ht->entries = (hash_entry_t *)AIRY_CALLOC(capacity, sizeof(hash_entry_t));
    if (!ht->entries) {
        ht->capacity = 0;
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "ht_init: allocation failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    ht->capacity = capacity;
    ht->count = 0;
    return 0;
}

static void ht_destroy(hash_table_t *ht)
{
    if (!ht->entries)
        return;
    for (size_t i = 0; i < ht->capacity; i++) {
        AIRY_FREE(ht->entries[i].key);
    }
    AIRY_FREE(ht->entries);
    ht->entries = NULL;
    ht->capacity = 0;
    ht->count = 0;
}

static bool ht_insert(hash_table_t *ht, const char *key, size_t index)
{
    if (!ht->entries || ht->count >= ht->capacity * 3 / 4)
        return false;
    unsigned long h = hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied) {
            /* P0: tombstone 槽位（deleted=true）可复用；复用前必须复位 deleted，
             * 否则会被 ht_lookup 跳过 */
            ht->entries[pos].key = AIRY_STRDUP(key);
            ht->entries[pos].index = index;
            ht->entries[pos].occupied = true;
            ht->entries[pos].deleted = false;
            ht->count++;
            return true;
        }
    }
    return false;
}

static ssize_t ht_lookup(hash_table_t *ht, const char *key)
{
    if (!ht->entries || ht->count == 0) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "ht_lookup: failed");
        return AIRY_ERR_UNKNOWN;
    }
    unsigned long h = hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied) {
            if (ht->entries[pos].deleted)
                continue;
            airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "hash_fn: failed");
            return AIRY_ERR_UNKNOWN;
        }
        if (strcmp(ht->entries[pos].key, key) == 0)
            return (ssize_t)ht->entries[pos].index;
    }
    airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "if: failed");
    return AIRY_ERR_UNKNOWN;
}

static void __attribute__((unused)) ht_remove(hash_table_t *ht, const char *key)
{
    if (!ht->entries || ht->count == 0)
        return;
    unsigned long h = hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied) {
            if (ht->entries[pos].deleted)
                continue;
            return;
        }
        if (strcmp(ht->entries[pos].key, key) == 0) {
            AIRY_FREE(ht->entries[pos].key);
            ht->entries[pos].key = NULL;
            ht->entries[pos].occupied = false;
            ht->entries[pos].index = 0;
            /* P0: 用 tombstone 标记删除，保持探测链连续，
             * 否则后续 ht_lookup 会因空槽提前终止而漏查元素 */
            ht->entries[pos].deleted = true;
            ht->count--;
            return;
        }
    }
}

static void __attribute__((unused)) ht_update(hash_table_t *ht, const char *key, size_t new_index)
{
    ssize_t idx = ht_lookup(ht, key);
    if (idx >= 0) {
        ht->entries[(size_t)idx].index = new_index;
    }
}

typedef struct {
    char *task_id;
    char *input;
    size_t input_len;
    int status;
    char *result;
    uint32_t timeout_ms;
    time_t created_at;
} task_entry_t;

typedef struct {
    char *session_id;
    char *metadata;
    time_t created_at;
    time_t last_accessed;
} session_entry_t;

static struct {
    task_entry_t *tasks;
    size_t task_count;
    hash_table_t task_index;
    session_entry_t *sessions;
    size_t session_count;
    hash_table_t session_index;
    uint64_t total_tasks_submitted;
    /* Telemetry 遥测字段：memory/agent 实际计数由 mem_d/agent_d 独立管理
     * （Phase 3 迁移），gateway 本地不统计，保持 0 输出。 */
    uint64_t record_count;
    uint64_t agent_count;
    uint64_t total_memory_writes;
    airy_mtx_t mutex;
    bool initialized;
} g_runtime = {0};

static void __attribute__((constructor)) runtime_init(void)
{
    airy_mtx_init(&g_runtime.mutex);

    const char *env;
    g_max_tasks = MAX_TASKS_DEFAULT;
    g_max_sessions = MAX_SESSIONS_DEFAULT;

    env = getenv("AIRY_MAX_TASKS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_max_tasks = (size_t)v;
    }
    env = getenv("AIRY_MAX_SESSIONS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_max_sessions = (size_t)v;
    }
    /* Phase 3：memory/agent 容量由 mem_d/agent_d 守护进程独立管理，
     * AIRY_MAX_RECORDS / AIRY_MAX_AGENTS 环境变量转发至对应 daemon 解析。 */

    g_runtime.tasks = (task_entry_t *)AIRY_CALLOC(g_max_tasks, sizeof(task_entry_t));
    g_runtime.sessions = (session_entry_t *)AIRY_CALLOC(g_max_sessions, sizeof(session_entry_t));
    if (!g_runtime.tasks || !g_runtime.sessions) {
        AIRY_LOG_ERROR("syscall_router: runtime_init calloc failed");
        AIRY_FREE(g_runtime.tasks);
        AIRY_FREE(g_runtime.sessions);
        g_runtime.tasks = NULL;
        g_runtime.sessions = NULL;
        return;
    }
    if (ht_init(&g_runtime.task_index, g_max_tasks * 2) != 0 ||
        ht_init(&g_runtime.session_index, g_max_sessions * 2) != 0) {
        ht_destroy(&g_runtime.task_index);
        ht_destroy(&g_runtime.session_index);
        AIRY_FREE(g_runtime.tasks);
        AIRY_FREE(g_runtime.sessions);
        g_runtime.tasks = NULL;
        g_runtime.sessions = NULL;
        return;
    }
    g_runtime.initialized = true;
}

static void __attribute__((destructor)) runtime_cleanup(void)
{

    for (size_t i = 0; i < g_runtime.task_count; i++) {
        AIRY_FREE(g_runtime.tasks[i].task_id);
        AIRY_FREE(g_runtime.tasks[i].input);
        AIRY_FREE(g_runtime.tasks[i].result);
    }

    for (size_t i = 0; i < g_runtime.session_count; i++) {
        AIRY_FREE(g_runtime.sessions[i].session_id);
        AIRY_FREE(g_runtime.sessions[i].metadata);
    }

    airy_mtx_destroy(&g_runtime.mutex);
    ht_destroy(&g_runtime.task_index);
    ht_destroy(&g_runtime.session_index);
    AIRY_FREE(g_runtime.tasks);
    AIRY_FREE(g_runtime.sessions);
    g_runtime.tasks = NULL;
    g_runtime.sessions = NULL;
    g_runtime.initialized = false;
}

static const char *generate_uuid(void)
{
    static char uuid[37];
    static uint64_t counter = 0;
    snprintf(uuid, sizeof(uuid), "agentrt-%016llx-%08llx", (unsigned long long)time(NULL),
             (unsigned long long)++counter);
    return uuid;
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

/* Memory 管理 — Phase 3：thin IPC client 转发至 mem_d 守护进程
 *
 * 保持原 airy_sys_memory_* 函数签名与 ABI 兼容；运行时通过 Unix socket
 * 向 mem_d 发送 JSON-RPC 请求（mem.write/search/get/delete），解析响应后
 * 按原 C ABI 返回。daemon 不可达时返回 AIRY_ERR_FAIL，由调用方降级处理。 */
airy_err_t airy_sys_memory_write(const void *data, size_t len, const char *metadata,
                                 char **out_record_id)
{
    if (!data || !len || !out_record_id)
        return AIRY_ERR_INVALID_PARAM;

    *out_record_id = NULL;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "data", (const char *)data);
    if (metadata && metadata[0] != '\0') {
        cJSON *meta = cJSON_Parse(metadata);
        if (meta)
            cJSON_AddItemToObject(params, "metadata", meta);
        else
            cJSON_AddStringToObject(params, "metadata", metadata);
    }

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_MEM_D_SOCKET, "write", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_memory_write: malformed result JSON");
        return AIRY_ERR_FAIL;
    }
    cJSON *rid = cJSON_GetObjectItem(result, "record_id");
    if (!cJSON_IsString(rid)) {
        cJSON_Delete(result);
        return AIRY_ERR_FAIL;
    }
    *out_record_id = AIRY_STRDUP(rid->valuestring);
    cJSON_Delete(result);
    return *out_record_id ? AIRY_OK : AIRY_ERR_OUT_OF_MEMORY;
}

airy_err_t airy_sys_memory_search(const char *query, uint32_t limit, char ***record_ids,
                                  float **scores, size_t *count)
{
    if (!record_ids || !scores || !count)
        return AIRY_ERR_INVALID_PARAM;

    *record_ids = NULL;
    *scores = NULL;
    *count = 0;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "query", query ? query : "");
    cJSON_AddNumberToObject(params, "limit", (double)limit);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_MEM_D_SOCKET, "search", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_memory_search: malformed result JSON");
        return AIRY_ERR_FAIL;
    }
    cJSON *arr = cJSON_GetObjectItem(result, "results");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(result);
        return AIRY_ERR_FAIL;
    }
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) {
        cJSON_Delete(result);
        return AIRY_OK;
    }

    *record_ids = (char **)AIRY_CALLOC((size_t)n, sizeof(char *));
    *scores = (float *)AIRY_CALLOC((size_t)n, sizeof(float));
    if (!*record_ids || !*scores) {
        AIRY_FREE(*record_ids);
        AIRY_FREE(*scores);
        *record_ids = NULL;
        *scores = NULL;
        cJSON_Delete(result);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *rid = cJSON_GetObjectItem(item, "record_id");
        cJSON *score = cJSON_GetObjectItem(item, "score");
        if (cJSON_IsString(rid))
            (*record_ids)[i] = AIRY_STRDUP(rid->valuestring);
        if (score && cJSON_IsNumber(score))
            (*scores)[i] = (float)score->valuedouble;
    }
    *count = (size_t)n;
    cJSON_Delete(result);
    return AIRY_OK;
}

airy_err_t airy_sys_memory_get(const char *record_id, void **out_data, size_t *out_len)
{
    if (!record_id || !out_data || !out_len)
        return AIRY_ERR_INVALID_PARAM;

    *out_data = NULL;
    *out_len = 0;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "record_id", record_id);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_MEM_D_SOCKET, "get", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_memory_get: malformed result JSON");
        return AIRY_ERR_FAIL;
    }
    cJSON *data = cJSON_GetObjectItem(result, "data");
    cJSON *len_field = cJSON_GetObjectItem(result, "length");
    if (!cJSON_IsString(data)) {
        cJSON_Delete(result);
        return AIRY_ERR_NOT_FOUND;
    }
    const char *str = data->valuestring;
    size_t slen = strlen(str);
    *out_data = AIRY_MALLOC(slen + 1);
    if (!*out_data) {
        cJSON_Delete(result);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    AIRY_MEMCPY(*out_data, str, slen);
    ((char *)*out_data)[slen] = '\0';
    *out_len = (len_field && cJSON_IsNumber(len_field)) ? (size_t)len_field->valuedouble : slen;
    cJSON_Delete(result);
    return AIRY_OK;
}

airy_err_t airy_sys_memory_delete(const char *record_id)
{
    if (!record_id)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "record_id", record_id);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_MEM_D_SOCKET, "delete", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    AIRY_FREE(result_str);
    return rc;
}

airy_err_t airy_sys_session_create(const char *metadata, char **out_session_id)
{
    if (!out_session_id)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    if (g_runtime.session_count >= g_max_sessions) {
        RUNTIME_UNLOCK();
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    session_entry_t *sess = &g_runtime.sessions[g_runtime.session_count++];
    sess->session_id = AIRY_STRDUP(generate_uuid());
    sess->metadata = metadata ? AIRY_STRDUP(metadata) : NULL;
    sess->created_at = time(NULL);
    sess->last_accessed = sess->created_at;
    *out_session_id = AIRY_STRDUP(sess->session_id);
    ht_insert(&g_runtime.session_index, sess->session_id, g_runtime.session_count - 1);
    RUNTIME_UNLOCK();
    return AIRY_OK;
}

airy_err_t airy_sys_session_get(const char *session_id, char **out_info)
{
    if (!session_id || !out_info)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    ssize_t idx = ht_lookup(&g_runtime.session_index, session_id);
    if (idx >= 0 && (size_t)idx < g_runtime.session_count) {
        g_runtime.sessions[idx].last_accessed = time(NULL);
        cJSON *info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "session_id", session_id);
        cJSON_AddStringToObject(info, "metadata",
                                g_runtime.sessions[idx].metadata ?
                                    g_runtime.sessions[idx].metadata :
                                    "");
        cJSON_AddNumberToObject(info, "age_seconds",
                                (double)(time(NULL) - g_runtime.sessions[idx].created_at));
        *out_info = cJSON_PrintUnformatted(info);
        cJSON_Delete(info);
        RUNTIME_UNLOCK();
        return AIRY_OK;
    }
    RUNTIME_UNLOCK();
    *out_info = AIRY_STRDUP("{}");
    return AIRY_ERR_NOT_FOUND;
}

airy_err_t airy_sys_session_close(const char *session_id)
{
    if (!session_id)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    ssize_t idx = ht_lookup(&g_runtime.session_index, session_id);
    if (idx >= 0 && (size_t)idx < g_runtime.session_count) {
        AIRY_FREE(g_runtime.sessions[idx].session_id);
        AIRY_FREE(g_runtime.sessions[idx].metadata);
        __builtin_memmove(&g_runtime.sessions[idx], &g_runtime.sessions[idx + 1],
                          (g_runtime.session_count - idx - 1) * sizeof(session_entry_t));
        g_runtime.session_count--;

        /* P0: 数组左移后，原 idx 之后所有 session 的下标均已变化，而
         * session_index 哈希表仍保存旧下标，会导致后续 session_get/close
         * 命中错误的会话（越界/UAF）。删除后重建整个 session_index。 */
        ht_destroy(&g_runtime.session_index);
        if (ht_init(&g_runtime.session_index, g_max_sessions * 2) != 0) {
            airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                             "session_index rebuild failed");
            RUNTIME_UNLOCK();
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < g_runtime.session_count; i++) {
            ht_insert(&g_runtime.session_index, g_runtime.sessions[i].session_id, i);
        }
        RUNTIME_UNLOCK();
        return AIRY_OK;
    }
    RUNTIME_UNLOCK();
    return AIRY_ERR_NOT_FOUND;
}

airy_err_t airy_sys_session_list(char ***sessions, size_t *count)
{
    if (!sessions || !count)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    *sessions = (char **)AIRY_CALLOC(g_runtime.session_count, sizeof(char *));
    if (!*sessions && g_runtime.session_count > 0) {
        RUNTIME_UNLOCK();
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < g_runtime.session_count; i++) {
        (*sessions)[i] = AIRY_STRDUP(g_runtime.sessions[i].session_id);
    }
    *count = g_runtime.session_count;
    RUNTIME_UNLOCK();
    return AIRY_OK;
}

/* Telemetry */
airy_err_t airy_sys_telemetry_metrics(char **out_metrics)
{
    if (!out_metrics)
        return AIRY_ERR_INVALID_PARAM;

    RUNTIME_LOCK();
    cJSON *metrics = cJSON_CreateObject();

    cJSON *runtime = cJSON_CreateObject();
    cJSON_AddNumberToObject(runtime, "total_tasks", (double)g_runtime.total_tasks_submitted);
    cJSON_AddNumberToObject(runtime, "active_tasks", (double)g_runtime.task_count);
    cJSON_AddNumberToObject(runtime, "memory_records", (double)g_runtime.record_count);
    cJSON_AddNumberToObject(runtime, "active_sessions", (double)g_runtime.session_count);
    cJSON_AddNumberToObject(runtime, "registered_agents", (double)g_runtime.agent_count);
    cJSON_AddNumberToObject(runtime, "memory_writes", (double)g_runtime.total_memory_writes);
    cJSON_AddItemToObject(metrics, "runtime", runtime);

    cJSON_AddStringToObject(metrics, "status", "operational");
    cJSON_AddNumberToObject(metrics, "uptime_seconds", (double)time(NULL));

    *out_metrics = cJSON_PrintUnformatted(metrics);
    RUNTIME_UNLOCK();
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

/* Agent 管理 — Phase 3：thin IPC client 转发至 agent_d 守护进程
 *
 * 保持原 airy_sys_agent_* 函数签名与 ABI 兼容；运行时通过 Unix socket
 * 向 agent_d 发送 JSON-RPC 请求（agent.spawn/terminate/invoke/list），
 * 解析响应后按原 C ABI 返回。daemon 不可达时返回 AIRY_ERR_FAIL。 */
airy_err_t airy_sys_agent_spawn(const char *spec, char **out_agent_id)
{
    if (!spec || !out_agent_id)
        return AIRY_ERR_INVALID_PARAM;

    *out_agent_id = NULL;

    cJSON *params = cJSON_CreateObject();

    cJSON *spec_obj = cJSON_Parse(spec);
    if (spec_obj) {
        cJSON_AddItemToObject(params, "agent_spec", spec_obj);
    } else {
        cJSON_AddStringToObject(params, "agent_spec", spec);
    }

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_AGENT_D_SOCKET, "spawn", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_agent_spawn: malformed result JSON");
        return AIRY_ERR_FAIL;
    }
    cJSON *aid = cJSON_GetObjectItem(result, "agent_id");
    if (!cJSON_IsString(aid)) {
        cJSON_Delete(result);
        return AIRY_ERR_FAIL;
    }
    *out_agent_id = AIRY_STRDUP(aid->valuestring);
    cJSON_Delete(result);
    return *out_agent_id ? AIRY_OK : AIRY_ERR_OUT_OF_MEMORY;
}

airy_err_t airy_sys_agent_terminate(const char *agent_id)
{
    if (!agent_id)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "agent_id", agent_id);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_AGENT_D_SOCKET, "terminate", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    AIRY_FREE(result_str);
    return rc;
}

airy_err_t airy_sys_agent_invoke(const char *agent_id, const char *input, size_t len,
                                 char **out_output)
{
    if (!agent_id || !input || !out_output)
        return AIRY_ERR_INVALID_PARAM;

    *out_output = NULL;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "agent_id", agent_id);

    char *input_str = (char *)AIRY_MALLOC(len + 1);
    if (!input_str) {
        cJSON_Delete(params);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    AIRY_MEMCPY(input_str, input, len);
    input_str[len] = '\0';
    cJSON_AddStringToObject(params, "input", input_str);
    AIRY_FREE(input_str);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *result_str = NULL;
    int rc = daemon_rpc_call(AIRY_AGENT_D_SOCKET, "invoke", params_str, &result_str,
                             AIRY_DAEMON_RPC_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_agent_invoke: malformed result JSON");
        return AIRY_ERR_FAIL;
    }
    cJSON *out_field = cJSON_GetObjectItem(result, "output");
    if (!cJSON_IsString(out_field)) {
        cJSON_Delete(result);
        return AIRY_ERR_FAIL;
    }
    *out_output = AIRY_STRDUP(out_field->valuestring);
    cJSON_Delete(result);
    return *out_output ? AIRY_OK : AIRY_ERR_OUT_OF_MEMORY;
}

airy_err_t airy_sys_agent_list(char ***agent_ids, size_t *count)
{
    if (!agent_ids || !count)
        return AIRY_ERR_INVALID_PARAM;

    *agent_ids = NULL;
    *count = 0;

    char *result_str = NULL;
    int rc =
        daemon_rpc_call(AIRY_AGENT_D_SOCKET, "list", "{}", &result_str, AIRY_DAEMON_RPC_TIMEOUT_MS);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_agent_list: malformed result JSON");
        return AIRY_ERR_FAIL;
    }
    cJSON *arr = cJSON_GetObjectItem(result, "agent_ids");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(result);
        return AIRY_ERR_FAIL;
    }
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) {
        cJSON_Delete(result);
        return AIRY_OK;
    }

    *agent_ids = (char **)AIRY_CALLOC((size_t)n, sizeof(char *));
    if (!*agent_ids) {
        cJSON_Delete(result);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsString(item))
            (*agent_ids)[i] = AIRY_STRDUP(item->valuestring);
    }
    *count = (size_t)n;
    cJSON_Delete(result);
    return AIRY_OK;
}
