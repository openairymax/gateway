// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file http_gateway_sse.c
 * @brief SSE frame management, content-reader state machine and route
 *        handlers for the HTTP gateway streaming subsystem.
 *
 * Originally part of http_gateway_routes.c (2300+ lines); extracted into
 * its own TU and later split across four files:
 *
 *   http_gateway_sse.c        — this file: SSE framing, MHD content_reader
 *                               state machine, route handlers, hall watch
 *   gateway_sse_stream.c      — llm_d streaming proxy (connect, request
 *                               build, RS frame parse, text extraction)
 *   gateway_sse_tool.c        — tool_d execute_tool, result formatting,
 *                               tool-loop guards
 *   gateway_sse_memory.c      — long-term memory inject / write-back
 *
 * Shared types and cross-module declarations live in
 * http_gateway_sse_internal.h (private to the SSE subsystem).
 */

#include "http_gateway_sse_internal.h"

/* Gateway-side hall event recording (write side of the SSoT event flow) */
#include "gateway_hall_store.h"

/* ── Hall event recording helper ───────────────────────────────────── */

/* 0.1.6h：单调毫秒时钟（空闲预算 deadline 用，避免系统时间回拨干扰）。 */
static unsigned long long gw_sse_now_ms(void)
{
#if defined(_WIN32)
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
#endif
}

static void gw_sse_record_event(gw_sse_ctx_t *sctx, const char *category, cJSON *content)
{
    if (!sctx || !sctx->task_id[0] || !category || !content)
        return;
    char *content_str = cJSON_PrintUnformatted(content);
    if (!content_str)
        return;
    (void)gw_hall_store_event(sctx->task_id, category, NULL, content_str);
    AIRY_FREE(content_str);
}

/* ── Last user-message extraction ──────────────────────────────────── */

static void gw_sse_user_prompt(const cJSON *messages, char *out, size_t out_sz)
{
    out[0] = '\0';
    if (!cJSON_IsArray(messages))
        return;
    int n = cJSON_GetArraySize(messages);
    for (int i = n - 1; i >= 0; i--) {
        cJSON *m = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (cJSON_IsString(role) && role->valuestring && strcmp(role->valuestring, "user") == 0) {
            cJSON *c = cJSON_GetObjectItem(m, "content");
            if (cJSON_IsString(c) && c->valuestring) {
                AIRY_STRNCPY_TERM(out, c->valuestring, out_sz);
                return;
            }
        }
    }
}

/* ── JSON error response (shared with gateway_sse_hall_watch.c) ──── */

int gw_sse_send_json_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                                  int status, const char *message)
{
    char err[256];
    int n = snprintf(err, sizeof(err), "{\"error\":{\"code\":%d,\"message\":\"%s\"}}", status,
                     message ? message : "error");
    if (n < 0 || n >= (int)sizeof(err)) {
        AIRY_STRNCPY_TERM(err, "{\"error\":{\"code\":500,\"message\":\"error\"}}", sizeof(err));
    }
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, status, err, strlen(err));
    int ret = MHD_NO;
    if (response) {
        ret = MHD_queue_response(connection, status, response);
        MHD_destroy_response(response);
    }
    atomic_fetch_add(&gateway->requests_failed, 1);
    return ret;
}

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
static int gw_sse_emit_reasoning(gw_sse_ctx_t *sctx, const char *content)
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
static int gw_sse_emit_usage(gw_sse_ctx_t *sctx)
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

/* Incremental recv from llm_d socket into the stream buffer.
 * Returns: 1 = data appended, 0 = EOF/fatal (stream terminated),
 *          2 = idle window (SO_RCVTIMEO 到期且未超总预算，流仍有效，
 *              调用方应发 keep-alive 帧并向 MHD 让出 CPU)。 */
static int gw_sse_recv_chunk(gw_sse_ctx_t *sctx)
{
    char tmp[4096];
    ssize_t n = recv(sctx->fd, tmp, sizeof(tmp), 0);
    if (n > 0) {
        if (gw_sse_stream_append(sctx, tmp, (size_t)n) != 0) {
            AIRY_LOG_WARN("SSE stream buffer append failed, terminating stream");
            sctx->stream_eof = 1;
            sctx->done = 1;
            close(sctx->fd);
            sctx->fd = -1;
            return -1;
        }
        return 1;
    }
    if (n == 0) {
        sctx->stream_eof = 1;
        close(sctx->fd);
        sctx->fd = -1;
        return 0;
    }
    if (errno == EINTR)
        return 1;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* 0.1.6h：轮询窗口内无数据。若超过 llm 流空闲总预算才判 EOF，
         * 否则返回 2 让调用方发 keep-alive——长思考间隙连接保持存活。 */
        if (sctx->idle_deadline_ms &&
            gw_sse_now_ms() >= sctx->idle_deadline_ms) {
            AIRY_LOG_WARN("SSE llm stream idle exceeded %ds budget, closing",
                     GW_SSE_RECV_TIMEOUT_S);
            sctx->stream_eof = 1;
            close(sctx->fd);
            sctx->fd = -1;
            return 0;
        }
        return 2;
    }
    sctx->stream_eof = 1;
    close(sctx->fd);
    sctx->fd = -1;
    return 0;
}

/* 0.1.6h：SSE keep-alive 注释帧（": keep-alive"），供 llm 空闲间隙续命。 */
static void gw_sse_emit_keepalive(gw_sse_ctx_t *sctx)
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
static void gw_sse_emit_tool_call(gw_sse_ctx_t *sctx, const char *tname,
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
static void gw_sse_emit_tool_result(gw_sse_ctx_t *sctx, const char *tname,
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

/* ── MHD content_reader: SSE tool-loop state machine ───────────────── */

static ssize_t gw_sse_content_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)cls;
    if (!sctx || sctx->done)
        return MHD_CONTENT_READER_END_OF_STREAM;
    if (max < 8)
        return MHD_CONTENT_READER_END_OF_STREAM;

    /* Drain the current step frame first (may span several callbacks). */
    if (sctx->step_buf && sctx->step_len > 0) {
        size_t n = sctx->step_len < max ? sctx->step_len : max;
        AIRY_MEMCPY(buf, sctx->step_buf, n);
        size_t rem = sctx->step_len - n;
        if (rem > 0) {
            AIRY_MEMCPY(sctx->step_buf, sctx->step_buf + n, rem);
            sctx->step_len = rem;
        } else {
            AIRY_FREE(sctx->step_buf);
            sctx->step_buf = NULL;
            sctx->step_len = 0;
        }
        return (ssize_t)n;
    }

    for (;;) {
        switch (sctx->phase) {
        case GW_SSE_PHASE_LLM_ROUND: {
            if (sctx->tool_round >= gw_sse_max_tool_loops()) {
                sctx->final_text = AIRY_STRDUP(GW_SSE_TOOL_LIMIT_MSG);
                if (!sctx->final_text) {
                    sctx->done = 1;
                    AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                    return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
                }
                sctx->final_len = strlen(sctx->final_text);
                sctx->final_pos = 0;
                sctx->phase = GW_SSE_PHASE_FINAL_TEXT;
                continue;
            }
            char *req_str = gw_sse_build_llm_request(sctx->model, sctx->messages, 1);
            if (!req_str) {
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }
            if (sctx->fd >= 0) {
                close(sctx->fd);
                sctx->fd = -1;
            }
            int sfd = gw_sse_stream_start(sctx->llm_sock, req_str, GW_SSE_RECV_TIMEOUT_S);
            AIRY_FREE(req_str);
            if (sfd < 0) {
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }
            sctx->fd = sfd;
            sctx->stream_len = 0;
            sctx->stream_eof = 0;
            sctx->text_streamed = 0;
            /* 0.1.6h：本轮流空闲总预算（90s），窗口内无数据时发 keep-alive */
            sctx->idle_deadline_ms =
                gw_sse_now_ms() + (unsigned long long)GW_SSE_RECV_TIMEOUT_S * 1000ULL;
            sctx->phase = GW_SSE_PHASE_LLM_STREAM;
            continue;
        }

        case GW_SSE_PHASE_LLM_STREAM: {
            for (;;) {
                gw_sse_stream_consume_frames(sctx);
                if (sctx->reasoning_delta) {
                    char *delta = sctx->reasoning_delta;
                    sctx->reasoning_delta = NULL;
                    int emitted = gw_sse_emit_reasoning(sctx, delta);
                    AIRY_FREE(delta);
                    if (emitted)
                        break;
                    continue;
                }
                if (gw_sse_stream_extract_text(sctx)) {
                    sctx->text_streamed = 1;
                    break;
                }
                if (sctx->stream_eof)
                    break;
                int rc = gw_sse_recv_chunk(sctx);
                if (rc < 0)
                    break;
                if (rc == 2) {
                    /* 0.1.6h：空闲窗口——发 keep-alive 续命并让出 MHD
                     * worker，避免 30s 连接空闲超时掐断长思考流 */
                    gw_sse_emit_keepalive(sctx);
                    break;
                }
                if (rc > 0)
                    continue;
                continue;
            }
            if (sctx->step_buf && sctx->step_len > 0)
                break;
            gw_sse_stream_consume_frames(sctx);
            if (sctx->reasoning_delta) {
                char *delta = sctx->reasoning_delta;
                sctx->reasoning_delta = NULL;
                int emitted = gw_sse_emit_reasoning(sctx, delta);
                AIRY_FREE(delta);
                if (emitted)
                    break;
            }
            if (sctx->tool_calls) {
                cJSON *assistant_msg = cJSON_CreateObject();
                if (assistant_msg) {
                    cJSON_AddStringToObject(assistant_msg, "role", "assistant");
                    cJSON_AddStringToObject(assistant_msg, "content", "");
                    if (sctx->reasoning && sctx->reasoning[0])
                        cJSON_AddStringToObject(assistant_msg, "reasoning_content",
                                                sctx->reasoning);
                    cJSON_AddItemToObject(assistant_msg, "tool_calls",
                                          cJSON_Duplicate(sctx->tool_calls, 1));
                    cJSON_AddItemToArray(sctx->messages, assistant_msg);
                }
                sctx->tc_count = cJSON_GetArraySize(sctx->tool_calls);
                sctx->tc_idx = 0;
                sctx->phase = GW_SSE_PHASE_EXEC_TOOLS;
                sctx->tool_round++;
                continue;
            }
            if (!sctx->final_text)
                sctx->final_text = AIRY_STRDUP("");
            if (!sctx->final_text) {
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }
            sctx->final_len = 0;
            sctx->final_pos = 0;
            sctx->phase = GW_SSE_PHASE_FINAL_TEXT;
            continue;
        }

        case GW_SSE_PHASE_EXEC_TOOLS: {
            if (!sctx->tool_calls || sctx->tc_idx >= sctx->tc_count) {
                if (sctx->tool_calls) {
                    cJSON_Delete(sctx->tool_calls);
                    sctx->tool_calls = NULL;
                }
                if (sctx->stash_result) {
                    AIRY_FREE(sctx->stash_result);
                    sctx->stash_result = NULL;
                }
                sctx->exec_done = 0;
                sctx->phase = GW_SSE_PHASE_LLM_ROUND;
                continue;
            }

            cJSON *tc = cJSON_GetArrayItem(sctx->tool_calls, sctx->tc_idx);
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *fn_name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *fn_args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            const char *tname = cJSON_IsString(fn_name) ? fn_name->valuestring : "";
            const char *targs = cJSON_IsString(fn_args) ? fn_args->valuestring : "{}";
            const char *tid = cJSON_IsString(tc_id) ? tc_id->valuestring : "";

            if (!sctx->exec_done) {
                gw_sse_emit_tool_call(sctx, tname, targs);

                char *result_text = NULL;
                gw_sse_execute_tool(sctx->tool_sock, tname, targs, &result_text);
                if (sctx->stash_result)
                    AIRY_FREE(sctx->stash_result);
                sctx->stash_result = result_text;
                sctx->exec_done = 1;
                break;
            }

            const char *res = sctx->stash_result ? sctx->stash_result : "Tool execution failed";
            int tool_ok = sctx->stash_result && res[0] != '\0' ? 1 : 0;
            char *feedback = gw_sse_feedback(res);
            gw_sse_emit_tool_result(sctx, tname, tid, res, tool_ok);

            gw_sse_append_tool_result(sctx->messages, tid, feedback ? feedback : res);
            AIRY_FREE(feedback);
            if (sctx->stash_result) {
                AIRY_FREE(sctx->stash_result);
                sctx->stash_result = NULL;
            }
            sctx->exec_done = 0;
            sctx->tc_idx++;
            cJSON *tevt = cJSON_CreateObject();
            if (tevt) {
                cJSON_AddStringToObject(tevt, "event", "tool_call");
                cJSON_AddStringToObject(tevt, "tool", tname);
                cJSON_AddNumberToObject(tevt, "ok", tool_ok);
                gw_sse_record_event(sctx, "chain", tevt);
                cJSON_Delete(tevt);
            }
            break;
        }

        case GW_SSE_PHASE_REASONING: {
            if (!sctx->reasoning) {
                sctx->phase = GW_SSE_PHASE_FINAL_TEXT;
                continue;
            }
            gw_sse_emit_reasoning(sctx, sctx->reasoning);
            AIRY_FREE(sctx->reasoning);
            sctx->reasoning = NULL;
            sctx->phase = GW_SSE_PHASE_FINAL_TEXT;
            break;
        }

        case GW_SSE_PHASE_FINAL_TEXT: {
            if (sctx->final_pos >= sctx->final_len) {
                if (!sctx->recorded_result) {
                    sctx->recorded_result = 1;
                    cJSON *revt = cJSON_CreateObject();
                    if (revt) {
                        cJSON_AddStringToObject(revt, "event", "chat_result");
                        cJSON_AddNumberToObject(revt, "ok", 1);
                        char tbuf[520];
                        AIRY_STRNCPY_TERM(tbuf, sctx->final_text ? sctx->final_text : "",
                                          sizeof(tbuf));
                        cJSON_AddStringToObject(revt, "text", tbuf);
                        gw_sse_record_event(sctx, "result", revt);
                        cJSON_Delete(revt);
                    }
                    gw_sse_mem_record(sctx);
                }
                if (sctx->usage_received && !sctx->usage_emitted) {
                    sctx->usage_emitted = 1;
                    if (gw_sse_emit_usage(sctx))
                        break;
                }
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }
            size_t n = sctx->final_len - sctx->final_pos;
            if (n > GW_SSE_TEXT_CHUNK)
                n = GW_SSE_TEXT_CHUNK;
            sctx->step_buf = (char *)AIRY_MALLOC(n + 9);
            if (!sctx->step_buf) {
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }
            AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
            AIRY_MEMCPY(sctx->step_buf + 6,
                        sctx->final_text ? sctx->final_text + sctx->final_pos : "", n);
            sctx->step_buf[6 + n] = '\n';
            sctx->step_buf[6 + n + 1] = '\n';
            sctx->step_buf[6 + n + 2] = '\0';
            sctx->step_len = 6 + n + 2;
            sctx->final_pos += n;
            break;
        }

        default:
            sctx->done = 1;
            AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
            return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
        }

        if (sctx->step_buf && sctx->step_len > 0) {
            size_t n = sctx->step_len < max ? sctx->step_len : max;
            AIRY_MEMCPY(buf, sctx->step_buf, n);
            size_t rem = sctx->step_len - n;
            if (rem > 0) {
                AIRY_MEMCPY(sctx->step_buf, sctx->step_buf + n, rem);
                sctx->step_len = rem;
            } else {
                AIRY_FREE(sctx->step_buf);
                sctx->step_buf = NULL;
                sctx->step_len = 0;
            }
            return (ssize_t)n;
        }
    }
}

/* ── MHD free_cb ───────────────────────────────────────────────────── */

static void gw_sse_content_free(void *cls)
{
    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)cls;
    if (!sctx)
        return;
    if (sctx->fd >= 0)
        close(sctx->fd);
    AIRY_FREE((void *)sctx->model);
    if (sctx->messages)
        cJSON_Delete(sctx->messages);
    if (sctx->tool_calls)
        cJSON_Delete(sctx->tool_calls);
    if (sctx->stash_result)
        AIRY_FREE(sctx->stash_result);
    AIRY_FREE(sctx->reasoning);
    AIRY_FREE(sctx->reasoning_delta);
    AIRY_FREE(sctx->final_text);
    AIRY_FREE(sctx->stream_buf);
    AIRY_FREE(sctx->step_buf);
    AIRY_FREE(sctx->user_prompt);
    AIRY_FREE(sctx);
}

/* ── POST /api/v1/chat/stream ──────────────────────────────────────── */

int handle_chat_stream_sse(http_gateway_t *gateway, struct MHD_Connection *connection,
                           http_request_context_t *context)
{
#ifndef _WIN32
    const char *body = context->body_buf;
    size_t body_len = context->body_len;
    if (!body || body_len == 0) {
        return gw_sse_send_json_error(gateway, connection, 400,
                                      "Request body required (model+messages or prompt)");
    }

    char *body_copy = (char *)AIRY_MALLOC(body_len + 1);
    if (!body_copy) {
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    AIRY_MEMCPY(body_copy, body, body_len);
    body_copy[body_len] = '\0';

    {
        char *clean = gw_sse_utf8_sanitize(body, body_len);
        if (clean) {
            AIRY_FREE(body_copy);
            body_copy = clean;
        }
    }

    cJSON *root = cJSON_Parse(body_copy);
    AIRY_FREE(body_copy);
    if (!root) {
        return gw_sse_send_json_error(gateway, connection, 400, "Invalid JSON body");
    }

    const char *model = GW_SSE_DEFAULT_MODEL;
    const cJSON *messages = NULL;
    const cJSON *prompt = NULL;
    const cJSON *params = cJSON_GetObjectItem(root, "params");
    const cJSON *cfg = cJSON_IsObject(params) ? params : root;
    const cJSON *m = cJSON_GetObjectItem(cfg, "model");
    if (cJSON_IsString(m) && m->valuestring && m->valuestring[0])
        model = m->valuestring;
    const cJSON *pmsg = cJSON_GetObjectItem(cfg, "messages");
    if (cJSON_IsArray(pmsg))
        messages = pmsg;
    const cJSON *pp = cJSON_GetObjectItem(cfg, "prompt");
    if (cJSON_IsString(pp) && pp->valuestring && pp->valuestring[0])
        prompt = pp;

    cJSON *history = NULL;
    if (messages && cJSON_GetArraySize(messages) > 0) {
        history = cJSON_Duplicate(messages, 1);
    } else if (prompt) {
        history = cJSON_CreateArray();
        if (history) {
            cJSON *msg = cJSON_CreateObject();
            if (!msg) {
                cJSON_Delete(history);
                history = NULL;
            } else {
                cJSON_AddStringToObject(msg, "role", "user");
                cJSON_AddStringToObject(msg, "content", prompt->valuestring);
                cJSON_AddItemToArray(history, msg);
            }
        }
    }
    char *model_dup = AIRY_STRDUP(model);
    cJSON_Delete(root);
    if (!model_dup || !history) {
        AIRY_FREE(model_dup);
        return gw_sse_send_json_error(gateway, connection, 400, "messages or prompt required");
    }

    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)AIRY_CALLOC(1, sizeof(gw_sse_ctx_t));
    if (!sctx) {
        cJSON_Delete(history);
        AIRY_FREE(model_dup);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    sctx->fd = -1;
    sctx->done = 0;
    sctx->phase = GW_SSE_PHASE_LLM_ROUND;
    sctx->model = model_dup;
    sctx->messages = history;
    sctx->tool_round = 0;
    gw_sse_resolve_llm_sock(sctx->llm_sock, sizeof(sctx->llm_sock));
    gw_sse_resolve_tool_sock(sctx->tool_sock, sizeof(sctx->tool_sock));

    {
        const cJSON *first_user = NULL;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, history) {
            cJSON *role = cJSON_GetObjectItem(item, "role");
            if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) {
                first_user = item;
                break;
            }
        }
        const cJSON *fc = first_user ? cJSON_GetObjectItem(first_user, "content") : NULL;
        const char *first_prompt = cJSON_IsString(fc) ? fc->valuestring : NULL;
        if (first_prompt && first_prompt[0]) {
            sctx->user_prompt = AIRY_STRDUP(first_prompt);
            gw_sse_mem_inject(history, first_prompt);
        }
    }

    gw_hall_task_id_now(sctx->task_id, sizeof(sctx->task_id));
    cJSON *start_evt = cJSON_CreateObject();
    if (start_evt) {
        cJSON_AddStringToObject(start_evt, "event", "chat_start");
        char pbuf[520];
        gw_sse_user_prompt(history, pbuf, sizeof(pbuf));
        cJSON_AddStringToObject(start_evt, "prompt", pbuf);
        gw_sse_record_event(sctx, "chain", start_evt);
        cJSON_Delete(start_evt);
    }

    struct MHD_Response *response =
        MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, GW_SSE_BLOCK_SIZE,
                                          gw_sse_content_reader, sctx, gw_sse_content_free);
    if (!response) {
        gw_sse_content_free(sctx);
        return gw_sse_send_json_error(gateway, connection, 500, "Failed to create stream response");
    }
    MHD_add_response_header(response, "Content-Type", "text/event-stream");
    MHD_add_response_header(response, "Cache-Control", "no-cache");
    MHD_add_response_header(response, "Connection", "keep-alive");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    gateway_apply_cors_headers(gateway, connection, response);

    atomic_fetch_add(&gateway->requests_total, 1);
    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    return ret;
#else
    (void)gateway;
    (void)connection;
    (void)context;
    return gw_sse_send_json_error(gateway, connection, 501,
                                  "SSE streaming unsupported on this platform");
#endif
}


