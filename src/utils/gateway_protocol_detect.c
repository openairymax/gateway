// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_protocol_detect.c
 * @brief Multi-protocol gateway request handler - protocol detection domain.
 *
 * Implements JSON signature based protocol detection (JSON-RPC / MCP /
 * A2A / LLM chat-completions) and the public detection predicates, single
 * responsibility. Split out of gateway_protocol_handler.c.
 */

#include "gateway_protocol_handler.h"

#include "gateway_protocol_handler_internal.h"

#include "airy_protocol_interface.h"

#include <cjson/cJSON.h>

#include <string.h>

static int has_key(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key) != NULL;
}

airy_protocol_type_t detect_protocol_internal(const char *request_data, size_t request_size)
{
    if (!request_data || request_size == 0)
        return AIRY_PROTOCOL_COUNT;

    cJSON *json = cJSON_ParseWithLength(request_data, request_size);
    if (!json)
        return AIRY_PROTOCOL_COUNT;

    airy_protocol_type_t type = AIRY_PROTOCOL_COUNT;
    const cJSON *jsonrpc = cJSON_GetObjectItemCaseSensitive(json, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(json, "method");

    if (cJSON_IsString(jsonrpc) && strcmp(jsonrpc->valuestring, "2.0") == 0 && method) {
        if (has_key(json, "MCP") || has_key(json, "mcp"))
            type = AIRY_PROTOCOL_MCP;
        else
            type = AIRY_PROTOCOL_JSON_RPC;
    } else if (has_key(json, "model") && (has_key(json, "messages") || has_key(json, "prompt"))) {
        type = proto_interface_parse_type("openai");
    } else if (has_key(json, "agent_id") && (has_key(json, "task_id") || has_key(json, "message"))) {
        type = AIRY_PROTOCOL_A2A;
    }

    cJSON_Delete(json);
    return type;
}

airy_protocol_type_t gateway_protocol_detect(const char *request_data, size_t request_size)
{
    return detect_protocol_internal(request_data, request_size);
}

int gateway_protocol_is_jsonrpc(const char *request_data, size_t request_size)
{
    return detect_protocol_internal(request_data, request_size) == AIRY_PROTOCOL_JSON_RPC ? 1 : 0;
}

int gateway_protocol_is_mcp(const char *request_data, size_t request_size)
{
    return detect_protocol_internal(request_data, request_size) == AIRY_PROTOCOL_MCP ? 1 : 0;
}

int gateway_protocol_is_a2a(const char *request_data, size_t request_size)
{
    return detect_protocol_internal(request_data, request_size) == AIRY_PROTOCOL_A2A ? 1 : 0;
}
