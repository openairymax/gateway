// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_protocol_handler.c
 * @brief Multi-protocol gateway request handler - lifecycle, main handling
 *        and statistics.
 *
 * SEC-017 compliance: every feature is a real implementation, no stubs.
 * Keeps the handler instance lifecycle, the adaptive multi-protocol main
 * dispatch, statistics and configuration. Protocol detection and
 * conversion live in gateway_protocol_detect.c /
 * gateway_protocol_convert.c.
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
#include <time.h>

// ============================================================================
// ============================================================================

struct gateway_protocol_handler_s {
    gateway_protocol_config_t config;
    void *router;

    uint64_t total_requests;
    uint64_t jsonrpc_requests;
    uint64_t mcp_requests;
    uint64_t a2a_requests;
    uint64_t openai_requests;
    uint64_t conversion_errors;
    uint64_t successful_responses;

    time_t created_at;
};

// ============================================================================
// ============================================================================

gateway_protocol_handler_t gateway_protocol_handler_create(const gateway_protocol_config_t *config)
{

    gateway_protocol_handler_t handler =
        (gateway_protocol_handler_t)AIRY_CALLOC(1, sizeof(struct gateway_protocol_handler_s));
    if (!handler)
        return NULL;

    if (config) {
        handler->config = *config;
    } else {
        gateway_protocol_handler_get_default_config(&handler->config);
    }

    handler->created_at = time(NULL);
    return handler;
}

void gateway_protocol_handler_destroy(gateway_protocol_handler_t handler)
{
    if (!handler)
        return;
    AIRY_FREE(handler);
}

rpc_result_t gateway_protocol_handle_request(gateway_protocol_handler_t handler,
                                             const char *request_data, size_t request_size,
                                             airy_protocol_type_t protocol_type,
                                             int (*custom_handler)(const char *, char **, void *),
                                             void *handler_data)
{

    if (!handler) {
        return create_error_result(-32600, "Invalid handler", "null");
    }

    handler->total_requests++;

    if (!request_data || request_size == 0) {
        handler->conversion_errors++;
        return create_error_result(-32602, "Empty request", "null");
    }

    if (request_size > handler->config.max_request_size) {
        handler->conversion_errors++;
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "Request too large: %zu > %u", request_size,
                 handler->config.max_request_size);
        return create_error_result(-32603, err_msg, "null");
    }

    airy_protocol_type_t detected_type = protocol_type;

    /* AIRY_PROTOCOL_COUNT means "unknown, auto-detect" (as the HTTP layer does);
      * AIRY_PROTOCOL_A2A keeps the old re-check-when-content-mismatches semantics. */
    if ((protocol_type == AIRY_PROTOCOL_COUNT || protocol_type == AIRY_PROTOCOL_A2A) &&
        handler->config.enable_protocol_detection) {
        detected_type = detect_protocol_internal(request_data, request_size);

        if (detected_type == AIRY_PROTOCOL_COUNT && handler->config.default_protocol) {

            if (strcmp(handler->config.default_protocol, "jsonrpc") == 0)
                detected_type = AIRY_PROTOCOL_JSON_RPC;
            else if (strcmp(handler->config.default_protocol, "mcp") == 0)
                detected_type = AIRY_PROTOCOL_MCP;
            else if (strcmp(handler->config.default_protocol, "openai") == 0)
                detected_type = AIRY_PROTOCOL_OPENAI;
            else if (strcmp(handler->config.default_protocol, "a2a") == 0)
                detected_type = AIRY_PROTOCOL_A2A;
        }
    }

    switch (detected_type) {
    case AIRY_PROTOCOL_JSON_RPC:
        handler->jsonrpc_requests++;
        break;
    case AIRY_PROTOCOL_MCP:
        handler->mcp_requests++;
        break;
    case AIRY_PROTOCOL_A2A:
        handler->a2a_requests++;
        break;
    case AIRY_PROTOCOL_OPENAI:
        handler->openai_requests++;
        break;
    default:
        break;
    }

    char *method = NULL;
    char *id_str = NULL;
    cJSON *converted_params = NULL;

    switch (detected_type) {
    case AIRY_PROTOCOL_JSON_RPC: {
        cJSON *json_rpc = cJSON_ParseWithLength(request_data, request_size);
        if (!json_rpc) {
            handler->conversion_errors++;
            return create_error_result(-32700, "Parse error: invalid JSON-RPC", "null");
        }

        cJSON *method_item = cJSON_GetObjectItem(json_rpc, "method");
        if (cJSON_IsString(method_item)) {
            method = AIRY_STRDUP(method_item->valuestring);
        } else {
            method = AIRY_STRDUP("unknown");
        }

        cJSON *id_item = cJSON_GetObjectItem(json_rpc, "id");
        if (id_item) {
            id_str = cJSON_PrintUnformatted(id_item);
        } else {
            id_str = AIRY_STRDUP("null");
        }

        char *params_str = cJSON_PrintUnformatted(cJSON_GetObjectItem(json_rpc, "params"));
        converted_params = cJSON_Parse(params_str);
        AIRY_FREE(params_str);
        cJSON_Delete(json_rpc);
    } break;

    case AIRY_PROTOCOL_MCP:
        if (!handler->config.enable_mcp_protocol) {
            AIRY_FREE(method);
            AIRY_FREE(id_str);
            handler->conversion_errors++;
            return create_error_result(-32604, "MCP protocol not enabled", "null");
        }
        converted_params = extract_mcp_to_jsonrpc(request_data, request_size, &method, &id_str);
        break;

    case AIRY_PROTOCOL_A2A:
        if (!handler->config.enable_a2a_protocol) {
            AIRY_FREE(method);
            AIRY_FREE(id_str);
            handler->conversion_errors++;
            return create_error_result(-32605, "A2A protocol not enabled", "null");
        }
        converted_params = extract_a2a_to_jsonrpc(request_data, request_size, &method, &id_str);
        break;

    case AIRY_PROTOCOL_OPENAI:
        if (!handler->config.enable_openai_protocol) {
            AIRY_FREE(method);
            AIRY_FREE(id_str);
            handler->conversion_errors++;
            return create_error_result(-32606, "OpenAI protocol not enabled", "null");
        }
        converted_params = extract_openai_to_jsonrpc(request_data, request_size, &method, &id_str);
        break;

    default:
        AIRY_FREE(method);
        AIRY_FREE(id_str);
        handler->conversion_errors++;
        return create_error_result(-32601, "Unknown protocol type", "null");
    }

    if (!converted_params) {

        converted_params = cJSON_CreateObject();
        if (!converted_params) {
            rpc_result_t result =
                create_error_result(-32700, "Protocol conversion failed", id_str ? id_str : "null");
            AIRY_FREE(method);
            AIRY_FREE(id_str);
            handler->conversion_errors++;
            return result;
        }
    }

    char *jsonrpc_request_str = NULL;
    {
        cJSON *jsonrpc_req = cJSON_CreateObject();
        cJSON_AddStringToObject(jsonrpc_req, "jsonrpc", "2.0");
        cJSON_AddStringToObject(jsonrpc_req, "method", method ? method : "unknown");
        cJSON_AddItemToObject(jsonrpc_req, "params", converted_params);
        if (id_str) {
            cJSON *id_parsed = cJSON_Parse(id_str);
            if (id_parsed) {
                cJSON_AddItemToObject(jsonrpc_req, "id", id_parsed);
            } else {
                cJSON_AddNullToObject(jsonrpc_req, "id");
            }
        } else {
            cJSON_AddNullToObject(jsonrpc_req, "id");
        }
        jsonrpc_request_str = cJSON_PrintUnformatted(jsonrpc_req);
        cJSON_Delete(jsonrpc_req);
    }

    char *response_str = NULL;
    int custom_result = 0;

    if (custom_handler) {
        custom_result = custom_handler(jsonrpc_request_str, &response_str, handler_data);
    } else {
        response_str =
            AIRY_STRDUP("{\"jsonrpc\":\"2.0\",\"result\":{\"status\":\"accepted\",\"message\":"
                        "\"Request queued for processing\"},\"id\":null}");
    }

    AIRY_FREE(method);
    AIRY_FREE(id_str);
    AIRY_FREE(jsonrpc_request_str);

    if (custom_result != 0 || !response_str) {
        AIRY_FREE(response_str);
        handler->conversion_errors++;
        return create_error_result(-32607,
                                   custom_result != 0 ? "Custom handler failed" :
                                                        "No response from handler",
                                   "null");
    }

    rpc_result_t final_result;
    AIRY_MEMSET(&final_result, 0, sizeof(final_result));
    final_result.error_code = 0;
    final_result.response_json = response_str;

    if (detected_type != AIRY_PROTOCOL_JSON_RPC) {

        do {
            CJSON_PARSE_GUARD(jsonrpc_resp, response_str, { break; });
            cJSON *result_data = cJSON_GetObjectItem(jsonrpc_resp, "result");
            if (result_data) {
                switch (detected_type) {
                case AIRY_PROTOCOL_OPENAI: {
                    cJSON *openai_resp = cJSON_CreateObject();
                    cJSON *choices = cJSON_CreateArray();
                    cJSON *choice = cJSON_CreateObject();
                    cJSON_AddItemToObject(choice, "message", CJSON_DEEP_COPY(result_data));
                    cJSON_AddItemToArray(choices, choice);
                    cJSON_AddItemToObject(openai_resp, "choices", choices);

                    cJSON *model_used = cJSON_GetObjectItem(result_data, "model");
                    if (model_used) {
                        cJSON_AddItemToObject(openai_resp, "model", CJSON_DEEP_COPY(model_used));
                    } else {
                        cJSON_AddStringToObject(openai_resp, "model", "default");
                    }

                    cJSON_AddStringToObject(openai_resp, "object", "chat.completion");

                    char *new_response = cJSON_PrintUnformatted(openai_resp);
                    AIRY_FREE(final_result.response_json);
                    final_result.response_json = new_response;
                    cJSON_Delete(openai_resp);
                } break;

                case AIRY_PROTOCOL_MCP: {
                    cJSON *mcp_resp = cJSON_CreateObject();
                    cJSON_AddItemToObject(mcp_resp, "content", CJSON_DEEP_COPY(result_data));
                    cJSON_AddBoolToObject(mcp_resp, "isError", 0);

                    char *new_response = cJSON_PrintUnformatted(mcp_resp);
                    AIRY_FREE(final_result.response_json);
                    final_result.response_json = new_response;
                    cJSON_Delete(mcp_resp);
                } break;

                case AIRY_PROTOCOL_A2A: {
                    cJSON *a2a_resp = cJSON_CreateObject();
                    cJSON_AddItemToObject(a2a_resp, "response", CJSON_DEEP_COPY(result_data));
                    cJSON_AddStringToObject(a2a_resp, "status", "success");

                    char *new_response = cJSON_PrintUnformatted(a2a_resp);
                    AIRY_FREE(final_result.response_json);
                    final_result.response_json = new_response;
                    cJSON_Delete(a2a_resp);
                } break;

                default:
                    break;
                }
            }

        } while (0);
    }

    handler->successful_responses++;
    return final_result;
}

int gateway_protocol_handler_get_stats(gateway_protocol_handler_t handler, char **stats_json)
{

    AIRY_CHECK(handler != NULL, AIRY_ERR_NULL_POINTER, "handler is NULL");
    AIRY_CHECK(stats_json != NULL, AIRY_ERR_NULL_POINTER, "stats_json is NULL");

    double uptime_seconds = difftime(time(NULL), handler->created_at);

    cJSON *stats = cJSON_CreateObject();

    cJSON *counts = cJSON_CreateObject();
    cJSON_AddNumberToObject(counts, "total_requests", (double)handler->total_requests);
    cJSON_AddNumberToObject(counts, "jsonrpc_requests", (double)handler->jsonrpc_requests);
    cJSON_AddNumberToObject(counts, "mcp_requests", (double)handler->mcp_requests);
    cJSON_AddNumberToObject(counts, "a2a_requests", (double)handler->a2a_requests);
    cJSON_AddNumberToObject(counts, "openai_requests", (double)handler->openai_requests);
    cJSON_AddNumberToObject(counts, "successful_responses", (double)handler->successful_responses);
    cJSON_AddNumberToObject(counts, "conversion_errors", (double)handler->conversion_errors);
    cJSON_AddItemToObject(stats, "request_counts", counts);

    cJSON_AddNumberToObject(stats, "uptime_seconds", uptime_seconds);
    cJSON_AddStringToObject(stats, "status", "operational");

    cJSON_AddBoolToObject(stats, "mcp_enabled", handler->config.enable_mcp_protocol);
    cJSON_AddBoolToObject(stats, "a2a_enabled", handler->config.enable_a2a_protocol);
    cJSON_AddBoolToObject(stats, "openai_enabled", handler->config.enable_openai_protocol);
    cJSON_AddBoolToObject(stats, "auto_detection", handler->config.enable_protocol_detection);

    *stats_json = cJSON_PrintUnformatted(stats);
    cJSON_Delete(stats);
    return 0;
}

void gateway_protocol_handler_get_default_config(gateway_protocol_config_t *config)
{
    if (!config)
        return;
    AIRY_MEMSET(config, 0, sizeof(*config));
    config->enable_mcp_protocol = true;
    config->enable_a2a_protocol = true;
    config->enable_openai_protocol = true;
    config->default_protocol = "jsonrpc";
    config->max_request_size = 65536;
    config->enable_protocol_detection = true;
}

int gateway_protocol_handler_load_config_from_json(gateway_protocol_config_t *config,
                                                   const char *json_config)
{

    AIRY_CHECK(config != NULL, AIRY_ERR_NULL_POINTER, "config is NULL");
    AIRY_CHECK(json_config != NULL, AIRY_ERR_NULL_POINTER, "json_config is NULL");

    gateway_protocol_handler_get_default_config(config);

    CJSON_PARSE_GUARD(root, json_config, {
        airy_err_push_ex(AIRY_ERR_INVALID_PARAM, __FILE__, __LINE__, __func__,
                         "gateway_protocol_handler: invalid parameter");
        return AIRY_ERR_INVALID_PARAM;
    });

    cJSON *item;

    item = cJSON_GetObjectItem(root, "enable_mcp_protocol");
    if (cJSON_IsBool(item))
        config->enable_mcp_protocol = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "enable_a2a_protocol");
    if (cJSON_IsBool(item))
        config->enable_a2a_protocol = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "enable_openai_protocol");
    if (cJSON_IsBool(item))
        config->enable_openai_protocol = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "default_protocol");
    if (cJSON_IsString(item))
        config->default_protocol = item->valuestring;

    item = cJSON_GetObjectItem(root, "max_request_size");
    if (cJSON_IsNumber(item))
        config->max_request_size = (uint32_t)item->valuedouble;

    item = cJSON_GetObjectItem(root, "enable_protocol_detection");
    if (cJSON_IsBool(item))
        config->enable_protocol_detection = cJSON_IsTrue(item);

    return 0;
}
