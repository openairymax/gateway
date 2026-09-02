// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_sse_frame.c
 * @brief SSE 事件 → 线上帧（wire-frame）纯编码器。
 *
 * Extracted from http_gateway_sse.c — owns the pure encoding of runtime
 * events into "data: <json>\n\n" SSE frames:
 *
 *   reasoning / usage / error / keep-alive / tool_call / tool_result
 *
 * Every emitter writes its result into sctx->step_buf / sctx->step_len,
 * which the MHD content-reader state machine in http_gateway_sse.c drains.
 * This module performs no socket IO and no control-flow decisions — it is a
 * stateless renderer over gw_sse_ctx_t.
 */

#include "http_gateway_sse_internal.h"

/* ── SSE frame builder ─────────────────────────────────────────────── */

static char *gw_sse_frame(const char *payload)
{
    size_t plen = strlen(payload);
    char *frame = (char *)AIRY_MALLOC(plen + 9);
    if (!frame)
        return NULL;
    AIRY_MEMCPY(frame, "data: ", 6);
    AIRY_MEMCPY(frame + 6, payload, plen);
    frame[6 + plen] = '\n';
    frame[6 + plen + 1] = '\n';
    frame[6 + plen + 2] = '\0';
    return frame;
}

/* Build a reasoning SSE event frame into step_buf (shared by 3 call sites
 * in the state machine: two in LLM_STREAM, one in REASONING phase). */
int gw_sse_emit_reasoning(gw_sse_ctx_t *sctx, const char *content)
{
    if (!content)
        return 0;
    cJSON *revt = cJSON_CreateObject();
    if (!revt)
        return 0;
    cJSON_AddStringToObject(revt, "__airy_evt", "reasoning");
    cJSON_AddStringToObject(revt, "content", content);
    if (sctx->model && sctx->model[0])
        cJSON_AddStringToObject(revt, "model", sctx->model);
    char *json = cJSON_PrintUnformatted(revt);
    cJSON_Delete(revt);
    if (!json)
        return 0;
    size_t jl = strlen(json);
    sctx->step_buf = (char *)AIRY_MALLOC(jl + 12);
    if (!sctx->step_buf) {
        AIRY_FREE(json);
        return 0;
    }
    AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
    AIRY_MEMCPY(sctx->step_buf + 6, json, jl);
    sctx->step_buf[6 + jl] = '\n';
    sctx->step_buf[6 + jl + 1] = '\n';
    sctx->step_buf[6 + jl + 2] = '\0';
    sctx->step_len = 6 + jl + 2;
    AIRY_FREE(json);
    return 1;
}

/* Build a usage SSE event frame into step_buf (emitted once before [DONE]). */
int gw_sse_emit_usage(gw_sse_ctx_t *sctx)
{
    cJSON *uevt = cJSON_CreateObject();
    if (!uevt)
        return 0;
    cJSON_AddStringToObject(uevt, "__airy_evt", "usage");
    cJSON_AddNumberToObject(uevt, "prompt_tokens", (double)sctx->prompt_tokens);
    cJSON_AddNumberToObject(uevt, "completion_tokens", (double)sctx->completion_tokens);
    cJSON_AddNumberToObject(uevt, "total_tokens", (double)sctx->total_tokens);
    cJSON_AddNumberToObject(uevt, "cost_usd", sctx->cost_usd);
    char *json = cJSON_PrintUnformatted(uevt);
    cJSON_Delete(uevt);
    if (!json)
        return 0;
    size_t jl = strlen(json);
    sctx->step_buf = (char *)AIRY_MALLOC(jl + 12);
    if (!sctx->step_buf) {
        AIRY_FREE(json);
        return 0;
    }
    AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
    AIRY_MEMCPY(sctx->step_buf + 6, json, jl);
    sctx->step_buf[6 + jl] = '\n';
    sctx->step_buf[6 + jl + 1] = '\n';
    sctx->step_buf[6 + jl + 2] = '\0';
    sctx->step_len = 6 + jl + 2;
    AIRY_FREE(json);
    return 1;
}

/* Build an error SSE event frame into step_buf (0.1.8：llm_d 错误信封 /
 * LLM 连接失败转为客户端可读的 __airy_evt:error 事件，杜绝原始 JSON-RPC
 * 错误对象被当正文 data: 帧上屏）。 */
int gw_sse_emit_error(gw_sse_ctx_t *sctx, const char *message)
{
    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return 0;
    cJSON_AddStringToObject(evt, "__airy_evt", "error");
    cJSON_AddStringToObject(evt, "message", message ? message : "LLM service error");
    char *json = cJSON_PrintUnformatted(evt);
    cJSON_Delete(evt);
    if (!json)
        return 0;
    sctx->step_buf = gw_sse_frame(json);
    AIRY_FREE(json);
    if (!sctx->step_buf)
        return 0;
    sctx->step_len = strlen(sctx->step_buf);
    return 1;
}

/* 0.1.6h：SSE keep-alive 注释帧（": keep-alive"），供 llm 空闲间隙续命。 */
void gw_sse_emit_keepalive(gw_sse_ctx_t *sctx)
{
    if (!sctx)
        return;
    sctx->step_buf = (char *)AIRY_MALLOC(sizeof(GW_SSE_KEEPALIVE_FRAME));
    if (!sctx->step_buf)
        return;
    AIRY_MEMCPY(sctx->step_buf, GW_SSE_KEEPALIVE_FRAME,
                sizeof(GW_SSE_KEEPALIVE_FRAME));
    sctx->step_len = sizeof(GW_SSE_KEEPALIVE_FRAME) - 1; /* 不含 NUL */
}

/* Build a tool_call SSE event frame into step_buf. */
void gw_sse_emit_tool_call(gw_sse_ctx_t *sctx, const char *tname,
                           const char *targs)
{
    cJSON *evt = cJSON_CreateObject();
    if (evt) {
        cJSON_AddStringToObject(evt, "__airy_evt", "tool_call");
        cJSON_AddStringToObject(evt, "tool", tname);
        cJSON *pargs = cJSON_Parse(targs);
        if (pargs)
            cJSON_AddItemToObject(evt, "args", pargs);
        else
            cJSON_AddStringToObject(evt, "args", targs);
        char *evt_str = cJSON_PrintUnformatted(evt);
        cJSON_Delete(evt);
        sctx->step_buf = evt_str ? gw_sse_frame(evt_str) : gw_sse_frame("{}");
        if (evt_str)
            AIRY_FREE(evt_str);
    } else {
        sctx->step_buf = gw_sse_frame("{}");
    }
    if (sctx->step_buf)
        sctx->step_len = strlen(sctx->step_buf);
}

/* Build a tool_result SSE event frame into step_buf. */
void gw_sse_emit_tool_result(gw_sse_ctx_t *sctx, const char *tname,
                             const char *tid, const char *res, int tool_ok)
{
    char *summary = gw_sse_summary(res);
    cJSON *evt = cJSON_CreateObject();
    if (evt) {
        cJSON_AddStringToObject(evt, "__airy_evt", "tool_result");
        cJSON_AddStringToObject(evt, "tool", tname);
        cJSON_AddStringToObject(evt, "call_id", tid);
        cJSON_AddNumberToObject(evt, "ok", tool_ok);
        cJSON_AddStringToObject(evt, "summary", summary ? summary : "");
        char *evt_str = cJSON_PrintUnformatted(evt);
        cJSON_Delete(evt);
        sctx->step_buf = evt_str ? gw_sse_frame(evt_str) : gw_sse_frame("{}");
        if (evt_str)
            AIRY_FREE(evt_str);
    } else {
        sctx->step_buf = gw_sse_frame("{}");
    }
    if (sctx->step_buf)
        sctx->step_len = strlen(sctx->step_buf);
    AIRY_FREE(summary);
}
