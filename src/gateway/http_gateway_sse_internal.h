// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http_gateway_sse_internal.h
 * @brief Shared types, constants and cross-module declarations for the
 *        SSE streaming subsystem (split across http_gateway_sse.c,
 *        gateway_sse_stream.c, gateway_sse_tool.c, gateway_sse_memory.c).
 *
 * This header is PRIVATE to the SSE subsystem — never install or expose it
 * outside the gateway translation unit.
 */

#ifndef HTTP_GATEWAY_SSE_INTERNAL_H
#define HTTP_GATEWAY_SSE_INTERNAL_H

#include "http_gateway_routes.h"

#include "gateway_rate_limiter.h"
#include "gateway_rpc_handler.h"
#include "gateway_utils.h"
#include "http_gateway.h"
#include "jsonrpc.h"
#include "logging.h"
#include "airy_memory.h"
#include "platform.h"
#include "syscall_router.h"
#include "syscall_router_internal.h"
#include "syscalls.h"

#include <microhttpd.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "atomic_compat.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define GW_SSE_DEFAULT_MODEL   "deepseek-v4-flash"
#define GW_SSE_RECV_TIMEOUT_S  90
#define GW_SSE_BLOCK_SIZE      1024
#define GW_SSE_DONE_EVENT      "data: [DONE]\n\n"
#define GW_SSE_MAX_TOOL_LOOPS  8
#define GW_SSE_TOOL_LIMIT_MSG  "任务步骤较多，已达执行轮数上限，请分步提问或精简要求后重试"
#define GW_SSE_TEXT_CHUNK      512
#define GW_SSE_SUMMARY_MAX     256
#define GW_SSE_TOOL_FEEDBACK_MAX 12288

/* llm_d complete_stream RS control-frame protocol (aligned with llm_d providers). */
#define GW_SSE_STREAM_RS          0x1e
#define GW_SSE_STREAM_TAG_TOOL    'T'
#define GW_SSE_STREAM_TAG_REASON  'R'
#define GW_SSE_STREAM_TAG_USAGE   'U'

/* ── Phase state machine ───────────────────────────────────────────── */

typedef enum {
    GW_SSE_PHASE_LLM_ROUND = 0,
    GW_SSE_PHASE_LLM_STREAM,
    GW_SSE_PHASE_EXEC_TOOLS,
    GW_SSE_PHASE_REASONING,
    GW_SSE_PHASE_FINAL_TEXT,
    GW_SSE_PHASE_DONE
} gw_sse_phase_t;

/* ── SSE callback context ──────────────────────────────────────────── */

typedef struct {
    char llm_sock[256];
    char tool_sock[256];
    int fd;
    int done;
    int phase;
    char *model;
    cJSON *messages;
    int tool_round;
    cJSON *tool_calls;
    int tc_count;
    int tc_idx;
    int exec_done;
    char *stash_result;
    char *reasoning;
    char *reasoning_delta;
    int reasoning_streamed;
    char *final_text;
    size_t final_len;
    size_t final_pos;
    char *stream_buf;
    size_t stream_len;
    size_t stream_cap;
    int stream_eof;
    int text_streamed;
    char *step_buf;
    size_t step_len;
    char task_id[64];
    int recorded_result;
    unsigned long long prompt_tokens;
    unsigned long long completion_tokens;
    unsigned long long total_tokens;
    double cost_usd;
    int usage_received;
    int usage_emitted;
    char *user_prompt;
    int mem_recorded;
} gw_sse_ctx_t;

/* ── gateway_sse_stream.c ──────────────────────────────────────────── */

void gw_sse_resolve_llm_sock(char *out, size_t out_size);
int  gw_sse_stream_start(const char *sock_path, const char *req_json, int timeout_s);
char *gw_sse_build_llm_request(const char *model, const cJSON *messages, int streaming);
int  gw_sse_stream_append(gw_sse_ctx_t *sctx, const char *data, size_t len);
int  gw_sse_stream_consume_frames(gw_sse_ctx_t *sctx);
int  gw_sse_stream_extract_text(gw_sse_ctx_t *sctx);
void gw_sse_text_accumulate(gw_sse_ctx_t *sctx, const char *text, size_t len);

/* ── gateway_sse_tool.c ────────────────────────────────────────────── */

void gw_sse_resolve_tool_sock(char *out, size_t out_size);
int  gw_sse_max_tool_loops(void);
int  gw_sse_execute_tool(const char *tool_sock, const char *name, const char *args_json,
                         char **out_text);
void gw_sse_append_tool_result(cJSON *messages, const char *tool_call_id,
                               const char *content);
char *gw_sse_summary(const char *text);
char *gw_sse_feedback(const char *text);

/* ── gateway_sse_memory.c ──────────────────────────────────────────── */

void gw_sse_mem_inject(cJSON *history, const char *prompt);
void gw_sse_mem_record(gw_sse_ctx_t *sctx);
char *gw_sse_utf8_sanitize(const char *s, size_t len);

/* ── Shared helpers (defined in http_gateway_sse.c) ────────────────── */

int gw_sse_send_json_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                           int status, const char *message);

#endif /* HTTP_GATEWAY_SSE_INTERNAL_H */
