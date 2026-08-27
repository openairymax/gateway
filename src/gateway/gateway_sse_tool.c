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

/* ── Generic Unix-socket JSON-RPC (used by tool execution) ─────────── */

static char *gw_sse_rpc(const char *sock_path, const char *req_json, int timeout_s)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    struct timeval tv = {timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t len = strlen(req_json);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_json + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        close(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > 1048576) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    close(fd);
    return resp;
#else
    (void)sock_path;
    (void)req_json;
    (void)timeout_s;
    return NULL;
#endif
}

/* ── Tool execution ────────────────────────────────────────────────── */

int gw_sse_execute_tool(const char *tool_sock, const char *name, const char *args_json,
                        char **out_text)
{
    *out_text = NULL;
    cJSON *req = cJSON_CreateObject();
    if (!req)
        return -1;
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", "execute_tool");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    cJSON_AddItemToObject(req, "params", params);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str)
        return -1;

    char *resp = gw_sse_rpc(tool_sock, req_str, GW_SSE_RECV_TIMEOUT_S);
    AIRY_FREE(req_str);
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
