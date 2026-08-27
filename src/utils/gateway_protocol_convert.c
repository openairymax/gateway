// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_protocol_convert.c
 * @brief Multi-protocol gateway request handler - protocol conversion domain.
 *
 * Implements protocol-to-JSON-RPC extraction (MCP/A2A/OpenAI), JSON-RPC
 * round-trip conversion and the backward-compatible JSON-RPC entry point,
 * single responsibility. Split out of gateway_protocol_handler.c.
 */

#include "gateway_protocol_handler.h"

#include "gateway_protocol_handler_internal.h"

#include "error.h"
#include "airy_memory.h"
#include "safe_string_utils.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

rpc_result_t create_error_result(int code, const char *message, const char *id_str)
{
    rpc_result_t result;
    AIRY_MEMSET(&result, 0, sizeof(result));
    result.error_code = code;
    /* error_message 只作诊断，不持有所有权（gateway_rpc_free 仅置 NULL），
     * 避免 strdup 泄漏且与 gateway_rpc_create_error 的字面量语义统一。 */
    result.error_message = message ? message : "Unknown error";

    cJSON *error_resp = cJSON_CreateObject();
    cJSON_AddStringToObject(error_resp, "jsonrpc", "2.0");

    cJSON *error_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(error_obj, "code", code);
    cJSON_AddStringToObject(error_obj, "message", message ? message : "Unknown error");
    cJSON_AddItemToObject(error_resp, "error", error_obj);

    if (id_str) {
        cJSON_AddRawToObject(error_resp, "id", id_str);
    } else {
        cJSON_AddNullToObject(error_resp, "id");
    }

    result.response_json = cJSON_PrintUnformatted(error_resp);
    cJSON_Delete(error_resp);

    return result;
}

cJSON *extract_openai_to_jsonrpc(const char *request_data, size_t request_size,
                                 char **out_method, char **out_id)
{
    cJSON *root = cJSON_ParseWithLength(request_data, request_size);
    if (!root)
        return NULL;

    const char *url_path =
        cJSON_GetObjectItem(root, "url") ? cJSON_GetObjectItem(root, "url")->valuestring : NULL;

    if (out_method) {
        /* P0: url_path is NULL when the request has no "url" field (or a non-string),
          * and strstr(NULL, ...) would crash; check NULL first and use the default path */
        if (!url_path) {
            *out_method = AIRY_STRDUP("openai.unknown");
        } else if (strstr(url_path, "/chat/completions")) {
            *out_method = AIRY_STRDUP("openai.chat.completions");
        } else if (strstr(url_path, "/completions")) {
            *out_method = AIRY_STRDUP("openai.completions");
        } else if (strstr(url_path, "/embeddings")) {
            *out_method = AIRY_STRDUP("openai.embeddings");
        } else {
            *out_method = AIRY_STRDUP(url_path);
        }
    }

    if (out_id) {
        cJSON *id_item = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(id_item)) {
            *out_id = AIRY_STRDUP(id_item->valuestring);
        } else if (cJSON_IsNumber(id_item)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", id_item->valueint);
            *out_id = AIRY_STRDUP(buf);
        } else {
            *out_id = AIRY_STRDUP("null");
        }
    }

    cJSON *params = cJSON_CreateObject();

    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (model)
        cJSON_AddItemToObject(params, "model", CJSON_DEEP_COPY(model));

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (messages)
        cJSON_AddItemToObject(params, "messages", CJSON_DEEP_COPY(messages));

    cJSON *prompt = cJSON_GetObjectItem(root, "prompt");
    if (prompt)
        cJSON_AddItemToObject(params, "prompt", CJSON_DEEP_COPY(prompt));

    cJSON *temperature = cJSON_GetObjectItem(root, "temperature");
    if (temperature)
        cJSON_AddItemToObject(params, "temperature", CJSON_DEEP_COPY(temperature));

    cJSON *max_tokens = cJSON_GetObjectItem(root, "max_tokens");
    if (max_tokens)
        cJSON_AddItemToObject(params, "max_tokens", CJSON_DEEP_COPY(max_tokens));

    cJSON_Delete(root);
    return params;
}

cJSON *extract_mcp_to_jsonrpc(const char *request_data, size_t request_size,
                              char **out_method, char **out_id)
{
    cJSON *root = cJSON_ParseWithLength(request_data, request_size);
    if (!root)
        return NULL;

    const char *method = cJSON_GetObjectItem(root, "method") ?
                             cJSON_GetObjectItem(root, "method")->valuestring :
                             NULL;

    if (out_method) {
        char mcp_method[256];
        snprintf(mcp_method, sizeof(mcp_method), "mcp.%s", method ? method : "unknown");
        *out_method = AIRY_STRDUP(mcp_method);
    }

    if (out_id) {
        cJSON *id_item = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsNumber(id_item)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)cJSON_GetNumberValue(id_item));
            *out_id = AIRY_STRDUP(buf);
        } else if (cJSON_IsString(id_item)) {
            *out_id = AIRY_STRDUP(id_item->valuestring);
        } else {
            *out_id = AIRY_STRDUP("null");
        }
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *result = params ? CJSON_DEEP_COPY(params) : cJSON_CreateObject();
    cJSON_Delete(root);
    return result;
}

cJSON *extract_a2a_to_jsonrpc(const char *request_data, size_t request_size,
                              char **out_method, char **out_id)
{
    cJSON *root = cJSON_ParseWithLength(request_data, request_size);
    if (!root)
        return NULL;

    if (out_method) {
        const char *action = cJSON_GetObjectItem(root, "action") ?
                                 cJSON_GetObjectItem(root, "action")->valuestring :
                                 "send";
        char a2a_method[256];
        snprintf(a2a_method, sizeof(a2a_method), "a2a.%s", action);
        *out_method = AIRY_STRDUP(a2a_method);
    }

    if (out_id) {
        const char *task_id = cJSON_GetObjectItem(root, "task_id") ?
                                  cJSON_GetObjectItem(root, "task_id")->valuestring :
                                  NULL;
        *out_id = AIRY_STRDUP(task_id ? task_id : "null");
    }

    cJSON *params = cJSON_CreateObject();

    cJSON *agent_id = cJSON_GetObjectItem(root, "agent_id");
    if (agent_id)
        cJSON_AddItemToObject(params, "target_agent", CJSON_DEEP_COPY(agent_id));

    cJSON *message = cJSON_GetObjectItem(root, "message");
    if (message)
        cJSON_AddItemToObject(params, "payload", CJSON_DEEP_COPY(message));

    cJSON_Delete(root);
    return params;
}

int gateway_protocol_convert_to_jsonrpc(gateway_protocol_handler_t handler,
                                        const char *request_data, size_t request_size,
                                        airy_protocol_type_t protocol_type, char **jsonrpc_out)
{

    AIRY_CHECK(handler != NULL, AIRY_ERR_NULL_POINTER, "handler is NULL");
    AIRY_CHECK(request_data != NULL, AIRY_ERR_NULL_POINTER, "request_data is NULL");
    AIRY_CHECK(jsonrpc_out != NULL, AIRY_ERR_NULL_POINTER, "jsonrpc_out is NULL");

    char *method = NULL;
    char *id_str = NULL;
    cJSON *params = NULL;

    switch (protocol_type) {
    case AIRY_PROTOCOL_MCP:
        params = extract_mcp_to_jsonrpc(request_data, request_size, &method, &id_str);
        break;
    case AIRY_PROTOCOL_A2A:
        params = extract_a2a_to_jsonrpc(request_data, request_size, &method, &id_str);
        break;
    case AIRY_PROTOCOL_OPENAI:
        params = extract_openai_to_jsonrpc(request_data, request_size, &method, &id_str);
        break;
    case AIRY_PROTOCOL_JSON_RPC:
        *jsonrpc_out = AIRY_STRNDUP(request_data, request_size);
        AIRY_FREE(method);
        AIRY_FREE(id_str);
        return *jsonrpc_out ? 0 : -2;
    default:
        airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__,
                         "gateway_protocol_handler: null pointer");
        return AIRY_ERR_NULL_POINTER;
    }

    if (!params) {
        AIRY_FREE(method);
        AIRY_FREE(id_str);
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "gateway_protocol_handler: out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    cJSON *jsonrpc_req = cJSON_CreateObject();
    cJSON_AddStringToObject(jsonrpc_req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(jsonrpc_req, "method", method ? method : "converted");
    cJSON_AddItemToObject(jsonrpc_req, "params", params);
    if (id_str) {
        cJSON *parsed_id = cJSON_Parse(id_str);
        if (parsed_id) {
            cJSON_AddItemToObject(jsonrpc_req, "id", parsed_id);
        } else {
            cJSON_AddNullToObject(jsonrpc_req, "id");
        }
    } else {
        cJSON_AddNullToObject(jsonrpc_req, "id");
    }

    *jsonrpc_out = cJSON_PrintUnformatted(jsonrpc_req);
    cJSON_Delete(jsonrpc_req);
    AIRY_FREE(method);
    AIRY_FREE(id_str);

    return *jsonrpc_out ? 0 : -5;
}

int gateway_protocol_convert_from_jsonrpc(gateway_protocol_handler_t handler,
                                          const char *jsonrpc_response,
                                          airy_protocol_type_t target_protocol,
                                          char **target_response)
{

    AIRY_CHECK(handler != NULL, AIRY_ERR_NULL_POINTER, "handler is NULL");
    AIRY_CHECK(jsonrpc_response != NULL, AIRY_ERR_NULL_POINTER, "jsonrpc_response is NULL");
    AIRY_CHECK(target_response != NULL, AIRY_ERR_NULL_POINTER, "target_response is NULL");

    CJSON_PARSE_GUARD(jsonrpc, jsonrpc_response, {
        airy_err_push_ex(AIRY_ERR_INVALID_PARAM, __FILE__, __LINE__, __func__,
                         "gateway_protocol_handler: invalid parameter");
        return AIRY_ERR_INVALID_PARAM;
    });

    cJSON *result = cJSON_GetObjectItem(jsonrpc, "result");
    if (!result) {

        airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__,
                         "gateway_protocol_handler: null pointer");
        return AIRY_ERR_NULL_POINTER;
    }

    switch (target_protocol) {
    case AIRY_PROTOCOL_OPENAI: {
        cJSON *openai = cJSON_CreateObject();
        cJSON *choices = cJSON_CreateArray();
        cJSON *choice = cJSON_CreateObject();

        cJSON_AddItemToObject(choice, "message", CJSON_DEEP_COPY(result));
        cJSON_AddItemToArray(choices, choice);
        cJSON_AddItemToObject(openai, "choices", choices);

        cJSON *model = cJSON_GetObjectItem(result, "model");
        if (model) {

            cJSON_AddItemToObject(openai, "model", CJSON_DEEP_COPY(model));
        } else {
            cJSON_AddStringToObject(openai, "model", "default");
        }
        cJSON_AddStringToObject(openai, "object", "chat.completion");

        *target_response = cJSON_PrintUnformatted(openai);
        cJSON_Delete(openai);
    } break;

    case AIRY_PROTOCOL_MCP: {
        cJSON *mcp = cJSON_CreateObject();

        cJSON_AddItemToObject(mcp, "content", CJSON_DEEP_COPY(result));
        cJSON_AddBoolToObject(mcp, "isError", 0);

        *target_response = cJSON_PrintUnformatted(mcp);
        cJSON_Delete(mcp);
    } break;

    case AIRY_PROTOCOL_A2A: {
        cJSON *a2a = cJSON_CreateObject();
        cJSON_AddItemToObject(a2a, "response", CJSON_DEEP_COPY(result));
        cJSON_AddStringToObject(a2a, "status", "success");

        *target_response = cJSON_PrintUnformatted(a2a);
        cJSON_Delete(a2a);
    } break;

    default:
        *target_response = AIRY_STRDUP(jsonrpc_response);
        break;
    }

    return *target_response ? 0 : -4;
}

rpc_result_t gateway_protocol_handle_jsonrpc(const cJSON *request,
                                             int (*handler)(const char *, char **, void *),
                                             void *handler_data)
{

    if (!request) {
        return create_error_result(-32600, "Invalid request", "null");
    }

    char *request_str = cJSON_PrintUnformatted((cJSON *)request);
    if (!request_str) {
        return create_error_result(-32700, "Failed to serialize request", "null");
    }

    gateway_protocol_config_t config;
    gateway_protocol_handler_get_default_config(&config);
    gateway_protocol_handler_t h = gateway_protocol_handler_create(&config);
    if (!h) {
        AIRY_FREE(request_str);
        return create_error_result(-32608, "Failed to create handler", "null");
    }

    rpc_result_t result =
        gateway_protocol_handle_request(h, request_str, strlen(request_str), AIRY_PROTOCOL_JSON_RPC,
                                        handler, handler_data);

    AIRY_FREE(request_str);
    gateway_protocol_handler_destroy(h);
    return result;
}
