// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_memory.c
 * @brief Syscall router memory domain (airy_sys_memory_* IPC forwarding and routing).
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

/**
  * @brief Route memory-management syscalls
 */
char *route_memory_methods(const char *method, cJSON *params, cJSON *request_id)
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

/* Memory management - Phase 3: thin IPC client forwarding to the mem_d daemon
 *
  * Keeps the airy_sys_memory_* signatures and ABI; at runtime, JSON-RPC requests
  * (mem.write/search/get/delete) go to mem_d over a Unix socket; responses
  * return via the original C ABI. AIRY_ERR_GENERIC_FAIL when the daemon is down; callers degrade. */
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
    /* 架构约束（2026-08-25）：统一经 syscall 派发（mem.write） */
    int rc = syscall_svc_call_unwrap("mem", "write", params_str,
                                     AIRY_DAEMON_RPC_TIMEOUT_MS, &result_str);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_memory_write: malformed result JSON");
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON *rid = cJSON_GetObjectItem(result, "record_id");
    if (!cJSON_IsString(rid)) {
        cJSON_Delete(result);
        return AIRY_ERR_GENERIC_FAIL;
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
    /* 架构约束（2026-08-25）：统一经 syscall 派发（mem.search） */
    int rc = syscall_svc_call_unwrap("mem", "search", params_str,
                                     AIRY_DAEMON_RPC_TIMEOUT_MS, &result_str);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_memory_search: malformed result JSON");
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON *arr = cJSON_GetObjectItem(result, "results");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(result);
        return AIRY_ERR_GENERIC_FAIL;
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
    /* 架构约束（2026-08-25）：统一经 syscall 派发（mem.get） */
    int rc = syscall_svc_call_unwrap("mem", "get", params_str,
                                     AIRY_DAEMON_RPC_TIMEOUT_MS, &result_str);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *result = cJSON_Parse(result_str);
    AIRY_FREE(result_str);
    if (!result) {
        SVC_LOG_ERROR("airy_sys_memory_get: malformed result JSON");
        return AIRY_ERR_GENERIC_FAIL;
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
    /* 架构约束（2026-08-25）：统一经 syscall 派发（mem.delete） */
    int rc = syscall_svc_call_unwrap("mem", "delete", params_str,
                                     AIRY_DAEMON_RPC_TIMEOUT_MS, &result_str);
    AIRY_FREE(params_str);
    AIRY_FREE(result_str);
    return rc;
}
