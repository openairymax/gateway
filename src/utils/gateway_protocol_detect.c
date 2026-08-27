// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_protocol_detect.c
 * @brief Multi-protocol gateway request handler - protocol detection domain.
 *
 * Implements JSON signature based protocol detection (JSON-RPC / MCP /
 * A2A / OpenAI) and the public detection predicates, single
 * responsibility. Split out of gateway_protocol_handler.c.
 */

#include "gateway_protocol_handler.h"

#include "gateway_protocol_handler_internal.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>

static const char *JSONRPC_SIGNATURES[]
    __attribute__((unused)) = {"\"jsonrpc\"", "\"method\"", "\"params\"", "\"id\"", NULL};

static const char *MCP_SIGNATURES[] __attribute__((
    unused)) = {"\"jsonrpc\": \"2.0\"", "\"method\"", "\"params\"", "\"MCP\"", "\"mcp\"", NULL};

static const char *OPENAI_SIGNATURES[]
    __attribute__((unused)) = {"\"model\"",           "\"messages\"",
                               "\"prompt\"",          "\"/v1/chat/completions\"",
                               "\"/v1/completions\"", NULL};

static const char *A2A_SIGNATURES[]
    __attribute__((unused)) = {"\"agent_id\"", "\"task_id\"",        "\"message\"",
                               "\"a2a\"",      "\"agent-to-agent\"", NULL};

static int json_field_equals(const char *json, const char *key, const char *value)
{
    if (!json || !key || !value)
        return 0;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\": \"%s\"", key, value);
    return strstr(json, pattern) != NULL ? 1 : 0;
}

static int json_field_exists(const char *json, const char *key)
{
    if (!json || !key)
        return 0;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern) != NULL ? 1 : 0;
}

static int is_valid_json(const char *data, size_t len)
{
    if (!data || len == 0)
        return 0;

    cJSON *json = cJSON_ParseWithLength(data, len);
    if (!json)
        return 0;
    cJSON_Delete(json);
    return 1;
}

airy_protocol_type_t detect_protocol_internal(const char *request_data, size_t request_size)
{

    if (!request_data || request_size == 0) {
        return AIRY_PROTOCOL_COUNT;
    }

    if (!is_valid_json(request_data, request_size)) {
        return AIRY_PROTOCOL_COUNT;
    }

    if (json_field_equals(request_data, "jsonrpc", "2.0") &&
        json_field_exists(request_data, "method")) {
        if (json_field_exists(request_data, "MCP") || json_field_exists(request_data, "mcp"))
            return AIRY_PROTOCOL_MCP;
        return AIRY_PROTOCOL_JSON_RPC;
    }

    if (json_field_exists(request_data, "model") &&
        (json_field_exists(request_data, "messages") || json_field_exists(request_data, "prompt")))
        return AIRY_PROTOCOL_OPENAI;

    if (json_field_exists(request_data, "agent_id") &&
        (json_field_exists(request_data, "task_id") || json_field_exists(request_data, "message")))
        return AIRY_PROTOCOL_A2A;

    return AIRY_PROTOCOL_COUNT;
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

int gateway_protocol_is_openai(const char *request_data, size_t request_size)
{
    return detect_protocol_internal(request_data, request_size) == AIRY_PROTOCOL_OPENAI ? 1 : 0;
}
