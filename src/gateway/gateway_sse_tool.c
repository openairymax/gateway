// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_sse_tool.c
 * @brief Tool execution loop: tool_d RPC, result formatting, conversation
 *        history management for the tool-call feedback cycle.
 *
 * Extracted from http_gateway_sse.c — owns the tool_d execute_tool wire
 * protocol, tool result truncation / summary, and the max-tool-loops guard.
 */

#include "http_gateway_sse_internal.h"

/* 0.1.16 B3: southbound A-IPC unified client face */
#include "biz/gateway_aipc_client.h"

/* ── Socket resolution ─────────────────────────────────────────────── */

void gw_sse_resolve_tool_sock(char *out, size_t out_size)
{
    const char *env = getenv("AIRY_TOOL_SOCK");
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_size);
        return;
    }
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir) {
        snprintf(out, out_size, "%s/tool.sock", run_dir);
    } else {
        AIRY_STRNCPY_TERM(out, "tool.sock", out_size);
    }
}

/* ── Max tool loops guard ──────────────────────────────────────────── */

int gw_sse_max_tool_loops(void)
{
    const char *env = getenv("AIRY_GW_SSE_MAX_TOOL_LOOPS");
    if (env && *env) {
        long v = strtol(env, NULL, 10);
        if (v > 0 && v <= 128)
            return (int)v;
    }
    return GW_SSE_MAX_TOOL_LOOPS;
}

/* ── Tool execution ────────────────────────────────────────────────── */

int gw_sse_execute_tool(const char *tool_sock, const char *name, const char *args_json,
                        char **out_text)
{
    *out_text = NULL;
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    /* 0.1.16 B3: the southbound transport lives in the unified A-IPC client
     * face (REQUEST/RESPONSE over gw_aipc_call); the former handwritten UDS
     * client (gw_sse_rpc) is gone. */
    char *resp = gw_aipc_call(tool_sock, "execute_tool", params_str,
                              GW_SSE_RECV_TIMEOUT_S * 1000);
    AIRY_FREE(params_str);
    if (!resp) {
        *out_text = AIRY_STRDUP("Tool service unreachable");
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *out_text = AIRY_STRDUP("Tool service returned invalid response");
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *err = cJSON_GetObjectItem(root, "error");
    int tool_ok = 0;
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        tool_ok = cJSON_IsNumber(success) && success->valueint != 0;
        if (tool_ok) {
            *out_text = AIRY_STRDUP(cJSON_IsString(output) && output->valuestring
                                        ? output->valuestring
                                        : "(no output)");
        } else {
            const char *e = cJSON_IsString(error) && error->valuestring ? error->valuestring
                                                                         : "execution failed";
            size_t elen = strlen(e) + 8;
            *out_text = (char *)AIRY_MALLOC(elen);
            if (*out_text)
                snprintf(*out_text, elen, "Error: %s", e);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        const char *m = cJSON_IsString(msg) && msg->valuestring ? msg->valuestring : "RPC error";
        size_t elen = strlen(m) + 8;
        *out_text = (char *)AIRY_MALLOC(elen);
        if (*out_text)
            snprintf(*out_text, elen, "Error: %s", m);
    } else {
        *out_text = AIRY_STRDUP("Tool service returned no result");
    }
    cJSON_Delete(root);
    return tool_ok ? 0 : -1;
}

/* ── Conversation history helpers ──────────────────────────────────── */

void gw_sse_append_tool_result(cJSON *messages, const char *tool_call_id,
                               const char *content)
{
    cJSON *tool_msg = cJSON_CreateObject();
    if (!tool_msg)
        return;
    cJSON_AddStringToObject(tool_msg, "role", "tool");
    cJSON_AddStringToObject(tool_msg, "tool_call_id", tool_call_id);
    cJSON_AddStringToObject(tool_msg, "content", content ? content : "Tool execution failed");
    cJSON_AddItemToArray(messages, tool_msg);
}

/* ── Result formatting ─────────────────────────────────────────────── */

char *gw_sse_summary(const char *text)
{
    if (!text)
        return AIRY_STRDUP("");
    size_t len = strlen(text);
    if (len <= GW_SSE_SUMMARY_MAX)
        return AIRY_STRDUP(text);
    char *sum = (char *)AIRY_MALLOC(GW_SSE_SUMMARY_MAX + 16);
    if (!sum)
        return AIRY_STRDUP("");
    AIRY_MEMCPY(sum, text, GW_SSE_SUMMARY_MAX);
    snprintf(sum + GW_SSE_SUMMARY_MAX, 16, "... (%zu bytes)", len);
    return sum;
}

char *gw_sse_feedback(const char *text)
{
    if (!text)
        return AIRY_STRDUP("Tool execution failed");
    size_t len = strlen(text);
    if (len <= GW_SSE_TOOL_FEEDBACK_MAX)
        return AIRY_STRDUP(text);
    char *fb = (char *)AIRY_MALLOC(GW_SSE_TOOL_FEEDBACK_MAX + 48);
    if (!fb)
        return AIRY_STRDUP(text);
    AIRY_MEMCPY(fb, text, GW_SSE_TOOL_FEEDBACK_MAX);
    snprintf(fb + GW_SSE_TOOL_FEEDBACK_MAX, 48, "\n...[truncated: %zu bytes]", len);
    return fb;
}
