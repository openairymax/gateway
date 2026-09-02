// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http_gateway_sse_internal.h
 * @brief Shared types, constants and cross-module declarations for the
 *        SSE streaming subsystem (split across http_gateway_sse.c,
 *        gateway_sse_frame.c, gateway_sse_stream.c, gateway_sse_tool.c,
 *        gateway_sse_memory.c, gateway_sse_hall_watch.c).
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
/* 0.1.6h：llm socket 轮询窗口（SO_RCVTIMEO）。阻塞 recv 按此粒度分片，
 * 空闲时向客户端发 SSE keep-alive 注释帧，防止 libmicrohttpd 30s 空闲
 * 超时掐断长对话流（表现为"回复为空/中途掉线"）。 */
#define GW_SSE_POLL_TIMEOUT_S  2
#define GW_SSE_KEEPALIVE_FRAME ": keep-alive\n\n"

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
    /* 0.1.6h：llm 流空闲总预算（单调毫秒时间戳）。每轮 LLM 流开始置
     * now + GW_SSE_RECV_TIMEOUT_S*1000；轮询窗口内无数据且未超预算则发
     * keep-alive 续命，超预算才判 EOF——保持原 90s 总超时语义。 */
    unsigned long long idle_deadline_ms;
    /* 0.1.8：llm_d 错误信封已转为 __airy_evt:error 帧发出（或 LLM 连接
     * 失败已发错误帧）。置位后状态机排空 step_buf 即发 [DONE] 收尾，
     * 不再进入工具循环/最终文本阶段。 */
    int llm_error;
} gw_sse_ctx_t;

/* ── gateway_sse_frame.c ─────────────────────────────────────────────
 * Pure SSE event → wire-frame encoders; write into sctx->step_buf and
 * return 1 on success (0 = OOM/alloc failure) for the int-returning ones. */

int  gw_sse_emit_reasoning(gw_sse_ctx_t *sctx, const char *content);
int  gw_sse_emit_usage(gw_sse_ctx_t *sctx);
int  gw_sse_emit_error(gw_sse_ctx_t *sctx, const char *message);
void gw_sse_emit_keepalive(gw_sse_ctx_t *sctx);
void gw_sse_emit_tool_call(gw_sse_ctx_t *sctx, const char *tname,
                           const char *targs);
void gw_sse_emit_tool_result(gw_sse_ctx_t *sctx, const char *tname,
                             const char *tid, const char *res, int tool_ok);

/* ── gateway_sse_stream.c ──────────────────────────────────────────── */

void gw_sse_resolve_llm_sock(char *out, size_t out_size);
int  gw_sse_stream_start(const char *sock_path, const char *req_json, int timeout_s);
char *gw_sse_build_llm_request(const char *model, const cJSON *messages, int streaming);
int  gw_sse_stream_append(gw_sse_ctx_t *sctx, const char *data, size_t len);
int  gw_sse_stream_consume_frames(gw_sse_ctx_t *sctx);
int  gw_sse_stream_extract_text(gw_sse_ctx_t *sctx);
void gw_sse_text_accumulate(gw_sse_ctx_t *sctx, const char *text, size_t len);
/* 0.1.8：检测 stream_buf 头部是否为 llm_d 的 JSON-RPC 错误信封（流式失败
 * 时 llm_d 把整个 error 对象裸写 socket，无 RS 控制帧）。返回 0=非信封，
 * 1=确认信封（message 提取到 out_msg），2=疑似信封但未完整（暂缓文本转发，
 * 等收全再判定，防把半截 JSON 当正文上屏）。 */
int  gw_sse_llm_error_envelope(gw_sse_ctx_t *sctx, char *out_msg, size_t out_len);

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
