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
 * dispatch, statistics and configuration. Body protocol detection lives in
 * the protocols mechanism core (proto_detect_body); conversion lives in
 * gateway_protocol_convert.c.
 */

#include "gateway_protocol_handler.h"

#include "gateway_protocol_handler_internal.h"

#include "protocol_detect.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// ============================================================================
// ============================================================================

struct gateway_protocol_handler_s {
    gateway_protocol_config_t config;

    uint64_t total_requests;
    uint64_t jsonrpc_requests;
    uint64_t mcp_requests;
    uint64_t a2a_requests;
    uint64_t llm_requests;
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

static rpc_result_t gw_fail(gateway_protocol_handler_t handler, int code, const char *msg,
                            char **method, char **id)
{
    rpc_result_t result = create_error_result(code, msg, *id ? *id : "null");
    AIRY_FREE(*method);
    AIRY_FREE(*id);
    *method = NULL;
    *id = NULL;
    handler->conversion_errors++;
    return result;
}

static airy_protocol_type_t gw_named(const char *name)
{
    if (!name)
        return AIRY_PROTOCOL_COUNT;
    if (strcmp(name, "jsonrpc") == 0)
        return AIRY_PROTOCOL_JSON_RPC;
    if (strcmp(name, "mcp") == 0)
        return AIRY_PROTOCOL_MCP;
    if (strcmp(name, "llm") == 0)
        return AIRY_PROTOCOL_LLM;
    if (strcmp(name, "a2a") == 0)
        return AIRY_PROTOCOL_A2A;
    return AIRY_PROTOCOL_COUNT;
}

static airy_protocol_type_t gw_classify(gateway_protocol_handler_t handler,
                                        const char *request_data, size_t request_size,
                                        airy_protocol_type_t protocol_type)
{
    airy_protocol_type_t detected = protocol_type;

    /* AIRY_PROTOCOL_COUNT means "unknown, auto-detect" (as the HTTP layer does);
      * AIRY_PROTOCOL_A2A keeps the old re-check-when-content-mismatches semantics. */
    if ((protocol_type == AIRY_PROTOCOL_COUNT || protocol_type == AIRY_PROTOCOL_A2A) &&
        handler->config.enable_protocol_detection) {
        detected = proto_detect_body(request_data, request_size);
        if (detected == AIRY_PROTOCOL_COUNT)
            detected = gw_named(handler->config.default_protocol);
    }

    if (detected == AIRY_PROTOCOL_JSON_RPC)
        handler->jsonrpc_requests++;
    else if (detected == AIRY_PROTOCOL_MCP)
        handler->mcp_requests++;
    else if (detected == AIRY_PROTOCOL_A2A)
        handler->a2a_requests++;
    else if (proto_is_llm(detected))
        handler->llm_requests++;
    return detected;
}

static int gw_rpc_parts(const char *request_data, size_t request_size, char **method, char **id,
                        cJSON **params)
{
    cJSON *doc = cJSON_ParseWithLength(request_data, request_size);
    if (!doc)
        return -32700;

    cJSON *method_item = cJSON_GetObjectItem(doc, "method");
    *method = AIRY_STRDUP(cJSON_IsString(method_item) ? method_item->valuestring : "unknown");

    cJSON *id_item = cJSON_GetObjectItem(doc, "id");
    *id = id_item ? cJSON_PrintUnformatted(id_item) : AIRY_STRDUP("null");

    char *params_str = cJSON_PrintUnformatted(cJSON_GetObjectItem(doc, "params"));
    *params = cJSON_Parse(params_str);
    AIRY_FREE(params_str);
    cJSON_Delete(doc);
    return 0;
}

static const char *gw_msg(int code)
{
    switch (code) {
    case -32700:
        return "Parse error: invalid JSON-RPC";
    case -32604:
        return "MCP protocol not enabled";
    case -32605:
        return "A2A protocol not enabled";
    case -32606:
        return "LLM protocol not enabled";
    default:
        return "Unknown protocol type";
    }
}

static int gw_convert(gateway_protocol_handler_t handler, airy_protocol_type_t detected,
                      const char *request_data, size_t request_size, char **method, char **id,
                      cJSON **params)
{
    if (detected == AIRY_PROTOCOL_JSON_RPC)
        return gw_rpc_parts(request_data, request_size, method, id, params);

    if (detected == AIRY_PROTOCOL_MCP) {
        if (!handler->config.enable_mcp_protocol)
            return -32604;
        *params = gw_mcp_to_rpc(request_data, request_size, method, id);
    } else if (detected == AIRY_PROTOCOL_A2A) {
        if (!handler->config.enable_a2a_protocol)
            return -32605;
        *params = gw_a2a_to_rpc(request_data, request_size, method, id);
    } else if (proto_is_llm(detected)) {
        if (!handler->config.enable_llm_protocol)
            return -32606;
        *params = gw_llm_to_rpc(request_data, request_size, method, id);
    } else {
        return -32601;
    }
    return 0;
}

static rpc_result_t gw_wrap(airy_protocol_type_t detected, char *response_str)
{
    cJSON *doc = cJSON_Parse(response_str);
    if (!doc)
        return (rpc_result_t){response_str, 0, NULL};

    char *wrapped = gw_proto_resp(detected, cJSON_GetObjectItem(doc, "result"));
    cJSON_Delete(doc);
    if (!wrapped)
        return (rpc_result_t){response_str, 0, NULL};

    AIRY_FREE(response_str);
    return (rpc_result_t){wrapped, 0, NULL};
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

    airy_protocol_type_t detected =
        gw_classify(handler, request_data, request_size, protocol_type);

    char *method = NULL;
    char *id_str = NULL;
    cJSON *converted_params = NULL;

    int cerr = gw_convert(handler, detected, request_data, request_size, &method, &id_str,
                          &converted_params);
    if (cerr != 0)
        return gw_fail(handler, cerr, gw_msg(cerr), &method, &id_str);

    if (!converted_params) {
        converted_params = cJSON_CreateObject();
        if (!converted_params)
            return gw_fail(handler, -32700, "Protocol conversion failed", &method, &id_str);
    }

    char *jsonrpc_request_str = gw_req_text(method ? method : "unknown", id_str, converted_params);
    AIRY_FREE(method);

    if (!custom_handler) {
        AIRY_FREE(jsonrpc_request_str);
        handler->conversion_errors++;
        rpc_result_t no_handler =
            create_error_result(-32603, "No request handler registered", id_str ? id_str : "null");
        AIRY_FREE(id_str);
        return no_handler;
    }

    char *response_str = NULL;
    int custom_result = custom_handler(jsonrpc_request_str, &response_str, handler_data);

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

    rpc_result_t final_result = {response_str, 0, NULL};

    if (detected != AIRY_PROTOCOL_JSON_RPC)
        final_result = gw_wrap(detected, response_str);

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
    cJSON_AddNumberToObject(counts, "llm_requests", (double)handler->llm_requests);
    cJSON_AddNumberToObject(counts, "successful_responses", (double)handler->successful_responses);
    cJSON_AddNumberToObject(counts, "conversion_errors", (double)handler->conversion_errors);
    cJSON_AddItemToObject(stats, "request_counts", counts);

    cJSON_AddNumberToObject(stats, "uptime_seconds", uptime_seconds);
    cJSON_AddStringToObject(stats, "status", "operational");

    cJSON_AddBoolToObject(stats, "mcp_enabled", handler->config.enable_mcp_protocol);
    cJSON_AddBoolToObject(stats, "a2a_enabled", handler->config.enable_a2a_protocol);
    cJSON_AddBoolToObject(stats, "llm_enabled", handler->config.enable_llm_protocol);
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
    config->enable_llm_protocol = true;
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

    item = cJSON_GetObjectItem(root, "enable_llm_protocol");
    if (cJSON_IsBool(item))
        config->enable_llm_protocol = cJSON_IsTrue(item);

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
