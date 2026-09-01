// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_sse_stream.c
 * @brief LLM streaming proxy: socket connect, request build, RS control-frame
 *        parsing, incremental text extraction and accumulation.
 *
 * Extracted from http_gateway_sse.c — owns the llm_d complete_stream wire
 * protocol (Unix-socket connect, JSON-RPC request, incremental recv, RS
 * frame parsing for tool_calls / reasoning / usage, text chunk forwarding).
 */

#include "http_gateway_sse_internal.h"

/* OpenAI tools schema shared with gateway_d (SSoT, one-to-one with tool_d) */
#include "airy_tool_schema.h"

/* ── Socket resolution ─────────────────────────────────────────────── */

void gw_sse_resolve_llm_sock(char *out, size_t out_size)
{
    const char *env = getenv("AIRY_LLM_SOCK");
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_size);
        return;
    }
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir) {
        snprintf(out, out_size, "%s/llm.sock", run_dir);
    } else {
        AIRY_STRNCPY_TERM(out, "llm.sock", out_size);
    }
}

/* ── LLM request builder ───────────────────────────────────────────── */

char *gw_sse_build_llm_request(const char *model, const cJSON *messages, int streaming)
{
    cJSON *llm_req = cJSON_CreateObject();
    if (!llm_req)
        return NULL;
    cJSON_AddStringToObject(llm_req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(llm_req, "id", 1);
    cJSON_AddStringToObject(llm_req, "method",
                            streaming ? "complete_stream" : "complete");
    cJSON *llm_params = cJSON_CreateObject();
    if (!llm_params) {
        cJSON_Delete(llm_req);
        return NULL;
    }
    cJSON_AddStringToObject(llm_params, "model", model);
    cJSON_AddItemToObject(llm_params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = cJSON_Parse(AIRY_TOOLS_JSON_SOURCE);
    if (tools) {
        cJSON_AddItemToObject(llm_params, "tools", tools);
    }
    long max_tokens = 4096;
    const char *env_mt = getenv("AIRY_GW_SSE_MAX_TOKENS");
    if (env_mt && *env_mt) {
        long v = strtol(env_mt, NULL, 10);
        if (v >= 256 && v <= 32768)
            max_tokens = v;
    }
    cJSON_AddNumberToObject(llm_params, "max_tokens", max_tokens);
    cJSON_AddNumberToObject(llm_params, "temperature", 0.7);
    cJSON_AddItemToObject(llm_req, "params", llm_params);
    char *req_str = cJSON_PrintUnformatted(llm_req);
    cJSON_Delete(llm_req);
    return req_str;
}

/* ── Stream connect ────────────────────────────────────────────────── */

int gw_sse_stream_start(const char *sock_path, const char *req_json, int timeout_s)
{
    /* timeout_s = llm 流空闲总预算（秒）；实际 recv 轮询窗口固定为
     * GW_SSE_POLL_TIMEOUT_S，空闲时由 content_reader 发 keep-alive，
     * 总预算由调用方经 sctx->idle_deadline_ms 监督。 */
    (void)timeout_s;
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = {GW_SSE_POLL_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t len = strlen(req_json);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_json + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)n;
    }
    return fd;
#else
    (void)sock_path;
    (void)req_json;
    (void)timeout_s;
    return -1;
#endif
}

/* ── Stream buffer management ──────────────────────────────────────── */

int gw_sse_stream_append(gw_sse_ctx_t *sctx, const char *data, size_t len)
{
    if (len == 0)
        return 0;
    if (sctx->stream_len + len + 1 > sctx->stream_cap) {
        size_t new_cap = sctx->stream_cap ? sctx->stream_cap : 8192;
        while (new_cap < sctx->stream_len + len + 1)
            new_cap *= 2;
        if (new_cap > 64 * 1024 * 1024)
            return -1;
        char *np = (char *)AIRY_REALLOC(sctx->stream_buf, new_cap);
        if (!np)
            return -1;
        sctx->stream_buf = np;
        sctx->stream_cap = new_cap;
    }
    AIRY_MEMCPY(sctx->stream_buf + sctx->stream_len, data, len);
    sctx->stream_len += len;
    sctx->stream_buf[sctx->stream_len] = '\0';
    return 0;
}

/* ── RS control-frame parser ───────────────────────────────────────── */

int gw_sse_stream_consume_frames(gw_sse_ctx_t *sctx)
{
    int consumed = 0;
    for (;;) {
        char *rs = sctx->stream_buf ? (char *)memchr(sctx->stream_buf, GW_SSE_STREAM_RS,
                                                     sctx->stream_len) : NULL;
        if (!rs)
            return consumed;
        size_t head = (size_t)(rs - sctx->stream_buf);
        if (head + 2 > sctx->stream_len)
            return consumed;
        char tag = rs[1];
        if (tag != GW_SSE_STREAM_TAG_TOOL && tag != GW_SSE_STREAM_TAG_REASON &&
            tag != GW_SSE_STREAM_TAG_USAGE) {
            sctx->stream_len -= head + 1;
            AIRY_MEMMOVE(sctx->stream_buf, rs + 1, sctx->stream_len);
            sctx->stream_buf[sctx->stream_len] = '\0';
            consumed = 1;
            continue;
        }
        char *end = (char *)memchr(rs + 2, GW_SSE_STREAM_RS,
                                   sctx->stream_len - (head + 2));
        if (!end)
            return consumed;
        size_t plen = (size_t)(end - (rs + 2));
        char *payload = (char *)AIRY_MALLOC(plen + 1);
        if (!payload)
            return consumed;
        AIRY_MEMCPY(payload, rs + 2, plen);
        payload[plen] = '\0';

        if (tag == GW_SSE_STREAM_TAG_TOOL) {
            cJSON *tc = cJSON_Parse(payload);
            if (tc && cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0) {
                if (sctx->tool_calls)
                    cJSON_Delete(sctx->tool_calls);
                sctx->tool_calls = tc;
            } else if (tc) {
                cJSON_Delete(tc);
            }
        } else if (tag == GW_SSE_STREAM_TAG_USAGE) {
            cJSON *u = cJSON_Parse(payload);
            if (u && cJSON_IsObject(u)) {
                cJSON *pt = cJSON_GetObjectItem(u, "prompt_tokens");
                cJSON *ct = cJSON_GetObjectItem(u, "completion_tokens");
                cJSON *tt = cJSON_GetObjectItem(u, "total_tokens");
                cJSON *cu = cJSON_GetObjectItem(u, "cost_usd");
                if (cJSON_IsNumber(pt))
                    sctx->prompt_tokens = (unsigned long long)pt->valuedouble;
                if (cJSON_IsNumber(ct))
                    sctx->completion_tokens = (unsigned long long)ct->valuedouble;
                if (cJSON_IsNumber(tt))
                    sctx->total_tokens = (unsigned long long)tt->valuedouble;
                else if (sctx->prompt_tokens || sctx->completion_tokens)
                    sctx->total_tokens = sctx->prompt_tokens + sctx->completion_tokens;
                if (cJSON_IsNumber(cu))
                    sctx->cost_usd = cu->valuedouble;
                sctx->usage_received = 1;
            }
            if (u)
                cJSON_Delete(u);
        } else {
            AIRY_FREE(sctx->reasoning_delta);
            sctx->reasoning_delta = payload;
            payload = NULL;
            size_t old = sctx->reasoning ? strlen(sctx->reasoning) : 0;
            size_t add = strlen(sctx->reasoning_delta);
            char *np = (char *)AIRY_REALLOC(sctx->reasoning, old + add + 1);
            if (np) {
                sctx->reasoning = np;
                AIRY_MEMCPY(sctx->reasoning + old, sctx->reasoning_delta, add);
                sctx->reasoning[old + add] = '\0';
            }
            sctx->reasoning_streamed = 1;
        }
        AIRY_FREE(payload);

        size_t total = (size_t)(end - sctx->stream_buf) + 1;
        sctx->stream_len -= total;
        AIRY_MEMMOVE(sctx->stream_buf, end + 1, sctx->stream_len);
        sctx->stream_buf[sctx->stream_len] = '\0';
        consumed = 1;
    }
}

/* ── Text accumulation (memory write-back evidence) ────────────────── */

void gw_sse_text_accumulate(gw_sse_ctx_t *sctx, const char *text, size_t len)
{
    if (!sctx || !text || len == 0)
        return;
    size_t old = sctx->final_text ? strlen(sctx->final_text) : 0;
    char *nb = (char *)AIRY_MALLOC(old + len + 1);
    if (!nb)
        return;
    if (old > 0)
        AIRY_MEMCPY(nb, sctx->final_text, old);
    AIRY_MEMCPY(nb + old, text, len);
    nb[old + len] = '\0';
    AIRY_FREE(sctx->final_text);
    sctx->final_text = nb;
}

/* ── llm_d error-envelope detection (0.1.8) ────────────────────────── */

/* llm_d 流式失败时把整个 JSON-RPC 错误对象（{"jsonrpc":..,"error":{..}}）
 * 裸写 socket 后关闭连接——无 RS 控制帧。此前网关把这段字节当正文
 * data: 帧透传，TUI 原样上屏（社区反馈：[Super Agent] 显示
 * {"jsonrpc":"2.0","id":1,"error":{"code":-32603,...}} 而非可读错误）。
 * 本函数在文本转发前检测信封：确认则提取 error.message 供上层转成
 * __airy_evt:error 事件帧。 */
int gw_sse_llm_error_envelope(gw_sse_ctx_t *sctx, char *out_msg, size_t out_len)
{
    if (out_len > 0)
        out_msg[0] = '\0';
    if (!sctx->stream_buf || sctx->stream_len == 0)
        return 0;
    if (sctx->stream_buf[0] != '{')
        return 0;
    /* 强签名：llm_d jsonrpc_build_error 的键序固定为插入序，输出必以
     * {"jsonrpc": 开头；正常文本块几乎不会以该前缀开始 → 直接放行。 */
    static const char sig[] = "{\"jsonrpc\":";
    size_t sig_len = sizeof(sig) - 1;
    if (sctx->stream_len < sig_len)
        return sctx->stream_eof ? 0 : 2; /* 签名未收全：暂缓，等 recv 补齐 */
    if (memcmp(sctx->stream_buf, sig, sig_len) != 0)
        return 0;
#ifdef AIRY_HAS_CJSON
    cJSON *root = cJSON_Parse(sctx->stream_buf);
    if (!root)
        return sctx->stream_eof ? 0 : 2; /* 流已结束仍畸形 → 按正文处理 */
    cJSON *err = cJSON_GetObjectItem(root, "error");
    int is_err = cJSON_IsObject(err);
    if (is_err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        if (cJSON_IsString(msg) && msg->valuestring && out_len > 0)
            AIRY_STRNCPY_TERM(out_msg, msg->valuestring, out_len);
    }
    cJSON_Delete(root);
    return is_err ? 1 : 0;
#else
    /* 无 cJSON 时按签名保守判定（消息置空，上层给通用文案） */
    return 1;
#endif
}

/* ── Incremental text extraction ───────────────────────────────────── */

int gw_sse_stream_extract_text(gw_sse_ctx_t *sctx)
{
    if (!sctx->stream_buf || sctx->stream_len == 0)
        return 0;
    char *rs = (char *)memchr(sctx->stream_buf, GW_SSE_STREAM_RS, sctx->stream_len);
    size_t take = rs ? (size_t)(rs - sctx->stream_buf) : sctx->stream_len;
    if (take == 0)
        return 0;
    gw_sse_text_accumulate(sctx, sctx->stream_buf, take);
    sctx->step_buf = (char *)AIRY_MALLOC(take + 9);
    if (!sctx->step_buf)
        return 0;
    AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
    AIRY_MEMCPY(sctx->step_buf + 6, sctx->stream_buf, take);
    sctx->step_buf[6 + take] = '\n';
    sctx->step_buf[6 + take + 1] = '\n';
    sctx->step_buf[6 + take + 2] = '\0';
    sctx->step_len = 6 + take + 2;
    sctx->stream_len -= take;
    AIRY_MEMMOVE(sctx->stream_buf, sctx->stream_buf + take, sctx->stream_len);
    sctx->stream_buf[sctx->stream_len] = '\0';
    return 1;
}
