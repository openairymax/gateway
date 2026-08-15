// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_agent.c
 * @brief Syscall router agent domain (airy_sys_agent_* IPC forwarding and routing).
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

/**
  * @brief Route Agent-management syscalls
 */
char *route_agent_methods(const char *method, cJSON *params, cJSON *request_id)
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

/* Agent management - Phase 3: thin IPC client forwarding to the agent_d daemon
 *
  * Keeps the airy_sys_agent_* signatures and ABI; at runtime, JSON-RPC requests
  * (agent.spawn/terminate/invoke/list) go to agent_d over a Unix socket,
  * and responses return via the original C ABI. AIRY_ERR_GENERIC_FAIL when the daemon is down. */
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
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON *aid = cJSON_GetObjectItem(result, "agent_id");
    if (!cJSON_IsString(aid)) {
        cJSON_Delete(result);
        return AIRY_ERR_GENERIC_FAIL;
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
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON *out_field = cJSON_GetObjectItem(result, "output");
    if (!cJSON_IsString(out_field)) {
        cJSON_Delete(result);
        return AIRY_ERR_GENERIC_FAIL;
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
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON *arr = cJSON_GetObjectItem(result, "agent_ids");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(result);
        return AIRY_ERR_GENERIC_FAIL;
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
