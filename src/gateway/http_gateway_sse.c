// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file http_gateway_sse.c
 * @brief SSE streaming subsystem of the HTTP gateway (chat/stream + hall/watch).
 *
 * Extracted from http_gateway_routes.c after that single file grew past
 * 2300 lines. The SSE chat streaming state machine (gw_sse_* helpers plus
 * the handle_chat_stream_sse / handle_hall_watch_sse route handlers) is a
 * cohesive single-responsibility module: llm_d streaming proxy + tool-loop
 * + SSE framing + long-term memory injection. Keeping it separate isolates
 * streaming logic from the plain HTTP route handlers (post_jsonrpc /
 * health / metrics / dispatch) that remain in http_gateway_routes.c.
 */

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

/* OpenAI tools schema shared with gateway_d (SSoT, one-to-one with tool_d) */
#include "gateway_tools_schema.h"

/* Gateway-side hall event recording (write side of the SSoT event flow) */
#include "gateway_hall_store.h"

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
#include <unistd.h> /* close() */
#endif

#include "atomic_compat.h"
#define GW_SSE_DEFAULT_MODEL "deepseek-v4-flash"
#define GW_SSE_RECV_TIMEOUT_S 90
#define GW_SSE_BLOCK_SIZE 1024
#define GW_SSE_DONE_EVENT "data: [DONE]\n\n"
/* 工具循环轮数上限（默认 8，可用环境变量 AIRY_GW_SSE_MAX_TOOL_LOOPS 覆盖）：
 * 超出后不再调用 LLM，把已收集内容作为最终回复发出，避免无限空转。 */
#define GW_SSE_MAX_TOOL_LOOPS 8
#define GW_SSE_TOOL_LIMIT_MSG "任务步骤较多，已达执行轮数上限，请分步提问或精简要求后重试"
#define GW_SSE_TEXT_CHUNK 512
#define GW_SSE_SUMMARY_MAX 256
/* Cap for tool results fed back to the LLM: web_fetch returns raw HTML that
 * can be tens/hundreds of KiB. Feeding it all burns tokens and drowns the
 * model; truncate with an explicit marker so the model knows content was cut
 * (bounds the complete request and keeps the tool loop cost sane). */
#define GW_SSE_TOOL_FEEDBACK_MAX 12288

/**
  * @brief SSE streaming response callback context
  *
  * cls for MHD_create_response_from_callback: holds the tool-loop state
  * machine. Freed by MHD's free_cb (gw_sse_content_free).
  *
  * The content_reader is a pull model: each MHD callback invocation produces
  * exactly one SSE frame. Tool execution (tool_d execute_tool) blocks inside
  * the callback like the plain llm_d recv does today; SSE is a long-lived
  * connection so a blocking step is acceptable.
  */
typedef enum {
    GW_SSE_PHASE_LLM_ROUND = 0, /* starting a complete_stream round (keep fd) */
    GW_SSE_PHASE_LLM_STREAM,    /* consuming llm_d streaming output incrementally */
    GW_SSE_PHASE_EXEC_TOOLS,    /* executing pending tool_calls one by one */
    GW_SSE_PHASE_REASONING,     /* emitting the model's reasoning_content (思考链) */
    GW_SSE_PHASE_FINAL_TEXT,    /* chunking the final reply text */
    GW_SSE_PHASE_DONE           /* [DONE] emitted */
} gw_sse_phase_t;

typedef struct {
    /* sockets (reconnected per step; only one live at a time) */
    char llm_sock[256];
    char tool_sock[256];
    int fd;           /* current llm.sock fd, -1 when idle */
    int done;
    int phase;
    /* request context */
    char *model;
    cJSON *messages;  /* conversation history (with tool feedback) */
    int tool_round;
    /* pending tool calls from the current LLM round */
    cJSON *tool_calls;
    int tc_count;
    int tc_idx;
    int exec_done;    /* 1 = current tool already executed (result stashed) */
    char *stash_result; /* tool result text for the pending tool_result event */
    /* reasoning (思考链) from the model, emitted before the final text */
    char *reasoning;
    /* 流中增量 reasoning（RS 'R' 帧逐个到达）：每块立即转发为
     * `__airy_evt:reasoning` 事件（实时思考链），同时累积到 reasoning */
    char *reasoning_delta;
    int reasoning_streamed; /* 思考链是否已随流式增量实时转发 */
    /* final text streaming */
    char *final_text;
    size_t final_len;
    size_t final_pos;
    /* llm_d complete_stream 增量缓冲：原始字节累积，逐步解析
     * （文本增量直接转发；RS 帧 'T' 解析 tool_calls、'R' 解析 reasoning） */
    char *stream_buf;
    size_t stream_len;
    size_t stream_cap;
    int stream_eof;   /* llm.sock 已读到 EOF */
    int text_streamed; /* 正文是否已随流式增量实时转发（FINAL_TEXT 不再重复） */
    /* in-flight step result (one SSE frame), written by the current phase */
    char *step_buf;
    size_t step_len;
    /* hall event recording (gateway_hall_store): task ID for this session
     * (generated when no client session_id is provided) + dedup flags */
    char task_id[64];
    int recorded_result;
    /* token usage from llm_d RS 'U' frame (2.1.1.5): parsed from the
     * stream trailer, forwarded as a `__airy_evt:usage` SSE event before
     * [DONE] so chat/stream clients see真实计费数据 */
    unsigned long long prompt_tokens;
    unsigned long long completion_tokens;
    unsigned long long total_tokens;
    double cost_usd;
    int usage_received; /* RS 'U' 帧已解析 */
    int usage_emitted;  /* usage SSE 事件已发送 */
    /* 长时记忆（memoryrovol，2.2.4 对话路径与 CLI 对齐）：
     * user_prompt = 本轮首个用户输入（记忆查询/写回的键）；
     * mem_recorded 保证会话只写回一次。 */
    char *user_prompt;
    int mem_recorded;
} gw_sse_ctx_t;

/* 长时记忆辅助函数前向声明（定义在文件下部，SSE 收尾路径在结构体
 * 之后即调用，须先声明避免 implicit declaration 与 static 冲突）。 */
static void gw_sse_mem_inject(cJSON *history, const char *prompt);
static void gw_sse_mem_record(gw_sse_ctx_t *sctx);

/**
  * @brief Resolve the llm_d socket path: env AIRY_LLM_SOCK -> airy_runtime_dir()/llm.sock
 *
  * Same origin as gw_resolve_daemon_sock in gateway_business_handler.c:
  * airy_runtime_dir() resolves $AIRY_HOME/run, defaulting to ~/.airymaxrt/run.
  * (airy_runtime_dir_socket uses a static buffer, so splice the path here.)
 */
static void gw_sse_resolve_llm_sock(char *out, size_t out_size)
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

/**
  * @brief 工具循环轮数上限（AIRY_GW_SSE_MAX_TOOL_LOOPS 环境变量可覆盖）。
  */
static int gw_sse_max_tool_loops(void)
{
    const char *env = getenv("AIRY_GW_SSE_MAX_TOOL_LOOPS");
    if (env && *env) {
        long v = strtol(env, NULL, 10);
        if (v > 0 && v <= 128)
            return (int)v;
    }
    return GW_SSE_MAX_TOOL_LOOPS;
}

/**
  * @brief 将无效 UTF-8 字节序列替换为 U+FFFD，返回 AIRY_MALLOC 新字符串。
  *
  * 输入可含任意字节（不做编码假设）；JSON 结构字符（引号/花括号等）均为
  * ASCII，清洗后 JSON 语法不受影响。调用方负责 AIRY_FREE。
  */
static char *gw_sse_utf8_sanitize(const char *s, size_t len)
{
    if (!s)
        return NULL;
    char *out = (char *)AIRY_MALLOC(len * 3 + 1); /* 最坏：每字节 → EF BF BD */
    if (!out)
        return NULL;
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need = 0;
        if (c < 0x80) {
            out[o++] = (char)c;
            i += 1;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
        }
        int valid = 1;
        for (size_t k = 1; need && k < need; ++k) {
            if (i + k >= len || ((unsigned char)s[i + k] & 0xC0) != 0x80) {
                valid = 0;
                break;
            }
        }
        if (need && valid) {
            for (size_t k = 0; k < need; ++k)
                out[o++] = s[i + k];
            i += need;
        } else {
            out[o++] = (char)0xEF;
            out[o++] = (char)0xBF;
            out[o++] = (char)0xBD;
            i += 1;
        }
    }
    out[o] = '\0';
    return out;
}

/**
  * @brief Resolve the tool_d socket path: env AIRY_TOOL_SOCK -> airy_runtime_dir()/tool.sock
 */
static void gw_sse_resolve_tool_sock(char *out, size_t out_size)
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

/* Record one hall event for an SSE session (best effort; a failed event
 * write must never disturb the stream). `content` must be a JSON object. */
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

/* Last user message content from the conversation history (SSE sessions may
 * arrive as OpenAI-format messages without an explicit prompt field). */
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

/**
  * @brief Send a JSON error response (non-streaming failure path: 400/500/502)
 */
static int gw_sse_send_json_error(http_gateway_t *gateway, struct MHD_Connection *connection,
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

/**
  * @brief Connect a Unix socket, send a JSON-RPC request and read the full response
  *
  * Used for the non-streaming llm.complete and tool.execute_tool round trips
  * inside the SSE tool loop. Returns the raw JSON-RPC response string
  * (AIRY_MALLOC) or NULL on failure.
 */
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

/**
  * @brief Build a llm.complete / llm.complete_stream JSON-RPC request
  *        (with the tool schema)
  *
  * `streaming` selects the method: "complete_stream" (incremental text +
  * RS control frames over the socket) vs "complete" (single JSON response).
  */
static char *gw_sse_build_llm_request(const char *model, const cJSON *messages, int streaming)
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
    cJSON *tools = cJSON_Parse(GW_TOOLS_JSON_SOURCE);
    if (tools) {
        cJSON_AddItemToObject(llm_params, "tools", tools);
    }
    /* 工具密集任务中 2048 输出上限可能截断 tool_calls/文本收尾，导致循环
     * 空转；默认 4096，可用 AIRY_GW_SSE_MAX_TOKENS 覆盖。 */
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

/**
  * @brief Connect to llm.sock, send the request, and return the open fd
  *        for incremental reads (complete_stream). Returns -1 on failure.
  */
static int gw_sse_stream_start(const char *sock_path, const char *req_json, int timeout_s)
{
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
    struct timeval tv = {timeout_s, 0};
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

/**
  * @brief Execute one tool via tool_d execute_tool
  * @return 0 on success (*out_text AIRY_MALLOC result text), non-zero on failure
  */
static int gw_sse_execute_tool(const char *tool_sock, const char *name, const char *args_json,
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

/**
  * @brief Build an SSE frame "data: <payload>\n\n" (AIRY_MALLOC)
 */
static char *gw_sse_frame(const char *payload)
{
    size_t plen = strlen(payload);
    /* "data: " (6) + payload + "\n\n" (2) + NUL (1) = plen + 9 */
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

/**
 * @brief Append raw bytes to the llm_d streaming buffer (auto-grow)
 * @return 0 on success, -1 on OOM
 */
static int gw_sse_stream_append(gw_sse_ctx_t *sctx, const char *data, size_t len)
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

/* llm_d complete_stream 控制帧分隔符（与 llm_d providers 对齐）：
 * 文本增量直接裸传；RS 帧仅三个：RS 'T' <tool_calls_json> RS、
 * RS 'R' <reasoning> RS 与 RS 'U' <usage_json> RS。RS (0x1E) 不出现
 * 在 JSON 与 LLM 文本中。 */
#define GW_SSE_STREAM_RS 0x1e
#define GW_SSE_STREAM_TAG_TOOL 'T'
#define GW_SSE_STREAM_TAG_REASON 'R'
#define GW_SSE_STREAM_TAG_USAGE 'U'

/**
 * @brief Parse one complete RS control frame out of the streaming buffer
 *
 * Extracts tool_calls ('T') or reasoning ('R') payload into the ctx.
 * @return 1 when a frame was consumed (buffer advanced); 0 when no complete
 *         frame is available yet (need more data)
 */
static int gw_sse_stream_consume_frames(gw_sse_ctx_t *sctx)
{
    int consumed = 0;
    for (;;) {
        char *rs = sctx->stream_buf ? (char *)memchr(sctx->stream_buf, GW_SSE_STREAM_RS,
                                                     sctx->stream_len) : NULL;
        if (!rs)
            return consumed; /* no frame boundary */
        size_t head = (size_t)(rs - sctx->stream_buf);
        if (head + 2 > sctx->stream_len)
            return consumed; /* need tag byte */
        char tag = rs[1];
        if (tag != GW_SSE_STREAM_TAG_TOOL && tag != GW_SSE_STREAM_TAG_REASON &&
            tag != GW_SSE_STREAM_TAG_USAGE) {
            /* Unknown frame: skip this RS byte and continue scanning. */
            sctx->stream_len -= head + 1;
            AIRY_MEMMOVE(sctx->stream_buf, rs + 1, sctx->stream_len);
            sctx->stream_buf[sctx->stream_len] = '\0';
            consumed = 1;
            continue;
        }
        /* Find the closing RS (payload runs from rs+2 .. end). */
        char *end = (char *)memchr(rs + 2, GW_SSE_STREAM_RS,
                                   sctx->stream_len - (head + 2));
        if (!end)
            return consumed; /* incomplete frame, wait for more data */
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
            /* 2.1.1.5 修复：llm_d 流式尾帧 RS 'U' 携带真实 token 消耗，
             * 解析并缓存在 ctx，FINAL_TEXT 结束时作为 usage SSE 事件透传
             * 给 chat/stream 客户端（此前该帧被当作未知帧丢弃，流式计费
             * 恒为 0）。cost_usd 由 llm_d 计费侧填充（缺失时保持 0）。 */
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
        } else { /* GW_SSE_STREAM_TAG_REASON */
            /* 增量 reasoning 帧：每块立即转发（实时思考链），同时累积 */
            AIRY_FREE(sctx->reasoning_delta);
            sctx->reasoning_delta = payload;
            payload = NULL;
            /* 累积完整 reasoning（供 tool 续轮 assistant 消息回传） */
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

/**
 * @brief Accumulate a text chunk into final_text (reply record for memory).
 *
 * 记忆写回证据：无工具路径的正文只经 SSE 实时转发（step_buf），从不落
 * final_text——此前 final_text 恒为空串，gw_sse_mem_record 的 <8 字符守卫
 * 永远跳过，长时记忆（L1）只字不写。此处把每个流式文本增量同时累积到
 * final_text，供收尾写回 "用户:/AgentRT:" 记录（与 CLI 共享召回）。
 * 多轮工具调用时累积全部轮次文本，记忆取最终完整回复，符合语义。
 */
static void gw_sse_text_accumulate(gw_sse_ctx_t *sctx, const char *text, size_t len)
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

/**
 * @brief Extract a text chunk before the first RS frame (real streaming)
 *
 * The bytes before the first RS boundary are plain LLM text; forward them
 * verbatim as an SSE "data: <text>\n\n" event so the client sees output
 * arrive incrementally instead of after the whole round completes.
 * @return 1 when a text event was built into step_buf; 0 when nothing
 *         (buffer empty or starts with an RS frame)
 */
static int gw_sse_stream_extract_text(gw_sse_ctx_t *sctx)
{
    if (!sctx->stream_buf || sctx->stream_len == 0)
        return 0;
    char *rs = (char *)memchr(sctx->stream_buf, GW_SSE_STREAM_RS, sctx->stream_len);
    size_t take = rs ? (size_t)(rs - sctx->stream_buf) : sctx->stream_len;
    if (take == 0)
        return 0; /* buffer starts with a control frame */
    /* 记忆写回证据：正文增量同步累积（无工具路径 final_text 不再为空） */
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

/**
  * @brief Append a tool result message to the conversation history
  */
static void gw_sse_append_tool_result(cJSON *messages, const char *tool_call_id,
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

/**
  * @brief Truncate a long result to a client-facing summary (AIRY_MALLOC)
  */
static char *gw_sse_summary(const char *text)
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

/**
 * @brief Tool result text fed back to the LLM as a role="tool" message
 *
 * Truncates oversized results (raw HTML from web_fetch) to
 * GW_SSE_TOOL_FEEDBACK_MAX bytes with an explicit "[truncated: N bytes]"
 * marker, so the model knows the content was cut. Returns AIRY_MALLOC.
 */
static char *gw_sse_feedback(const char *text)
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

/**
  * @brief MHD content_reader: drive the SSE tool-loop state machine
  *
  * MHD semantics (microhttpd.h): a return >0 is the number of bytes written
  * to buf; MHD_CONTENT_READER_END_OF_STREAM (-1) marks the stream end.
  * Each invocation produces exactly one SSE frame (the step_buf produced by
  * the current phase); phases are advanced step by step:
  *   LLM_ROUND   -> start llm.complete_stream (keep the socket open), then
  *                  LLM_STREAM consumes the incremental output: text chunks
  *                  are forwarded immediately (real streaming), RS frames
  *                  carry tool_calls ('T') and reasoning ('R'); on EOF, if
  *                  tools were requested switch to EXEC_TOOLS, else emit the
  *                  reasoning event then [DONE].
  *   EXEC_TOOLS  -> emit tool_call event (first invocation per tool), then
  *                  execute via tool.sock, feed back and emit tool_result;
  *                  loop through all pending calls, then back to LLM_ROUND.
  *   FINAL_TEXT  -> emit text chunks; on exhaustion emit [DONE], phase DONE.
  */
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
                /* 超过工具轮数上限：不再继续调用 LLM，把已收集的内容作为
                 * 最终回复发出（可读中文提示，替代原英文占位符）。 */
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
            /* 真流式：发起 llm.complete_stream 并保持 fd 打开，增量读取。
             * 文本增量实时转发（打字机动效），RS 控制帧解析
             * tool_calls（'T'）与 reasoning（'R'）。 */
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
            sctx->phase = GW_SSE_PHASE_LLM_STREAM;
            continue; /* fall into LLM_STREAM below */
        }

        case GW_SSE_PHASE_LLM_STREAM: {
            /* 增量读取 llm.sock：文本增量实时转发为 SSE 事件，RS 帧解析
             * tool_calls / reasoning。阻塞读取（SO_RCVTIMEO 兜底），每次
             * content_reader 调用转发一段文本或处理完一个控制帧。 */
            for (;;) {
                /* 1. 先消费缓冲中已有的完整 RS 控制帧（tool_calls/reasoning） */
                gw_sse_stream_consume_frames(sctx);
                /* 2. 增量 reasoning 帧：立即转发为思考链事件（实时可见）。
                 * 事件携带 model 字段：前端（TUI/CLI）据此区分双思考轨道
                 * （t2→[Dual Slow Think] / t1-f→[Dual Fast Think] /
                 * t1-p→[Dual Prof Think]，2.3.14）。 */
                if (sctx->reasoning_delta) {
                    char *delta = sctx->reasoning_delta;
                    sctx->reasoning_delta = NULL;
                    cJSON *revt = cJSON_CreateObject();
                    if (revt) {
                        cJSON_AddStringToObject(revt, "__airy_evt", "reasoning");
                        cJSON_AddStringToObject(revt, "content", delta);
                        if (sctx->model && sctx->model[0])
                            cJSON_AddStringToObject(revt, "model", sctx->model);
                        char *json = cJSON_PrintUnformatted(revt);
                        cJSON_Delete(revt);
                        if (json) {
                            size_t jl = strlen(json);
                            sctx->step_buf = (char *)AIRY_MALLOC(jl + 12);
                            if (sctx->step_buf) {
                                AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
                                AIRY_MEMCPY(sctx->step_buf + 6, json, jl);
                                sctx->step_buf[6 + jl] = '\n';
                                sctx->step_buf[6 + jl + 1] = '\n';
                                sctx->step_buf[6 + jl + 2] = '\0';
                                sctx->step_len = 6 + jl + 2;
                            }
                            AIRY_FREE(json);
                        }
                    }
                    AIRY_FREE(delta);
                    if (sctx->step_buf && sctx->step_len > 0)
                        break; /* 先返回 reasoning 事件 */
                    continue;
                }
                /* 3. 提取 RS 帧之前的文本增量 → 转发（真流式动效） */
                if (gw_sse_stream_extract_text(sctx)) {
                    sctx->text_streamed = 1;
                    break; /* step_buf 已填充，返回该帧 */
                }
                /* 4. 缓冲已空或只剩不完整帧：读更多数据 */
                if (sctx->stream_eof) {
                    /* 流已结束（EOF）→ 收尾 */
                    break;
                }
                char tmp[4096];
                ssize_t n = recv(sctx->fd, tmp, sizeof(tmp), 0);
                if (n > 0) {
                    /* P1: on append failure (buffer over 64MB / OOM) terminate
                     * the stream instead of silently dropping the chunk and
                     * spinning until the recv timeout hangs the SSE response. */
                    if (gw_sse_stream_append(sctx, tmp, (size_t)n) != 0) {
                        AIRY_LOG_WARN("SSE stream buffer append failed, terminating stream");
                        sctx->stream_eof = 1;
                        sctx->done = 1;
                        close(sctx->fd);
                        sctx->fd = -1;
                        break;
                    }
                    continue;
                }
                if (n == 0) {
                    /* EOF：llm_d complete_stream 无 JSON-RPC 包络，EOF 即结束 */
                    sctx->stream_eof = 1;
                    close(sctx->fd);
                    sctx->fd = -1;
                    continue;
                }
                /* n < 0 */
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* 超时：视为异常结束 */
                    sctx->stream_eof = 1;
                    close(sctx->fd);
                    sctx->fd = -1;
                    continue;
                }
                /* 其他错误：结束 */
                sctx->stream_eof = 1;
                close(sctx->fd);
                sctx->fd = -1;
                continue;
            }
            /* 退出循环：若 step_buf 有文本帧 → 返回它 */
            if (sctx->step_buf && sctx->step_len > 0)
                break;
            /* 流结束收尾：解析剩余 RS 帧，判定结果 */
            gw_sse_stream_consume_frames(sctx);
            if (sctx->reasoning_delta) {
                /* 最后一个 reasoning 增量块尚未转发（带 model 轨道，2.3.14） */
                char *delta = sctx->reasoning_delta;
                sctx->reasoning_delta = NULL;
                cJSON *revt = cJSON_CreateObject();
                if (revt) {
                    cJSON_AddStringToObject(revt, "__airy_evt", "reasoning");
                    cJSON_AddStringToObject(revt, "content", delta);
                    if (sctx->model && sctx->model[0])
                        cJSON_AddStringToObject(revt, "model", sctx->model);
                    char *json = cJSON_PrintUnformatted(revt);
                    cJSON_Delete(revt);
                    if (json) {
                        size_t jl = strlen(json);
                        sctx->step_buf = (char *)AIRY_MALLOC(jl + 12);
                        if (sctx->step_buf) {
                            AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
                            AIRY_MEMCPY(sctx->step_buf + 6, json, jl);
                            sctx->step_buf[6 + jl] = '\n';
                            sctx->step_buf[6 + jl + 1] = '\n';
                            sctx->step_buf[6 + jl + 2] = '\0';
                            sctx->step_len = 6 + jl + 2;
                        }
                        AIRY_FREE(json);
                    }
                }
                AIRY_FREE(delta);
                if (sctx->step_buf && sctx->step_len > 0)
                    break;
            }
            if (sctx->tool_calls) {
                /* Preserve the assistant round in history (for the next LLM call).
                 * DeepSeek thinking mode 要求续轮回传 reasoning_content，
                 * 否则上游 400（与 cli_chat.c 工具续轮语义一致）。 */
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
                continue; /* fall through to EXEC_TOOLS below */
            }
            /* 无工具：最终正文已实时转发；思考链已随流式增量实时转发
             * （reasoning_streamed），不再走 REASONING 阶段（避免重复），
             * 直接 FINAL_TEXT（final_text 已由流式增量累积 → 立即 [DONE]）。
             * 注意：不得覆盖已累积的回复正文——gw_sse_mem_record 依赖它
             * 写回长时记忆（final_text 曾恒为空串导致记忆不落库）。 */
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
                /* All pending tools done: next LLM round. */
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
                /* First pass: emit the tool_call event, then execute
                 * immediately (blocking) and stash the result so the next
                 * callback can emit the tool_result frame. */
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

                char *result_text = NULL;
                gw_sse_execute_tool(sctx->tool_sock, tname, targs, &result_text);
                if (sctx->stash_result)
                    AIRY_FREE(sctx->stash_result);
                sctx->stash_result = result_text;
                sctx->exec_done = 1;
                break; /* return the tool_call frame */
            }

            /* Second pass: emit the tool_result frame and feed back. */
            const char *res = sctx->stash_result ? sctx->stash_result : "Tool execution failed";
            int tool_ok = sctx->stash_result && res[0] != '\0' ? 1 : 0;
            char *summary = gw_sse_summary(res);
            char *feedback = gw_sse_feedback(res);
            cJSON *evt2 = cJSON_CreateObject();
            if (evt2) {
                cJSON_AddStringToObject(evt2, "__airy_evt", "tool_result");
                cJSON_AddStringToObject(evt2, "tool", tname);
                cJSON_AddStringToObject(evt2, "call_id", tid);
                cJSON_AddNumberToObject(evt2, "ok", tool_ok);
                cJSON_AddStringToObject(evt2, "summary", summary ? summary : "");
                char *evt_str = cJSON_PrintUnformatted(evt2);
                cJSON_Delete(evt2);
                sctx->step_buf = evt_str ? gw_sse_frame(evt_str) : gw_sse_frame("{}");
                if (evt_str)
                    AIRY_FREE(evt_str);
            } else {
                sctx->step_buf = gw_sse_frame("{}");
            }
            if (sctx->step_buf)
                sctx->step_len = strlen(sctx->step_buf);

            gw_sse_append_tool_result(sctx->messages, tid,
                                      feedback ? feedback : res);
            AIRY_FREE(feedback);
            AIRY_FREE(summary);
            if (sctx->stash_result) {
                AIRY_FREE(sctx->stash_result);
                sctx->stash_result = NULL;
            }
            sctx->exec_done = 0;
            sctx->tc_idx++;
            /* Record one tool-call event per executed tool (deduped: this is
             * the second pass, i.e. exactly once per tool). tool_ok was
             * evaluated above before stash_result is freed. */
            cJSON *tevt = cJSON_CreateObject();
            if (tevt) {
                cJSON_AddStringToObject(tevt, "event", "tool_call");
                cJSON_AddStringToObject(tevt, "tool", tname);
                cJSON_AddNumberToObject(tevt, "ok", tool_ok);
                gw_sse_record_event(sctx, "chain", tevt);
                cJSON_Delete(tevt);
            }
            break; /* return the tool_result frame */
        }

        case GW_SSE_PHASE_REASONING: {
            /* 思考链（reasoning_content）透传为单条 SSE 事件：
             *   data: {"__airy_evt":"reasoning","content":"...","model":"..."}\n\n
             * model 字段供前端区分双思考轨道（2.3.14）；完成后转入 FINAL_TEXT。 */
            if (!sctx->reasoning) {
                sctx->phase = GW_SSE_PHASE_FINAL_TEXT;
                continue;
            }
            cJSON *revt = cJSON_CreateObject();
            if (revt) {
                cJSON_AddStringToObject(revt, "__airy_evt", "reasoning");
                cJSON_AddStringToObject(revt, "content", sctx->reasoning);
                if (sctx->model && sctx->model[0])
                    cJSON_AddStringToObject(revt, "model", sctx->model);
                char *json = cJSON_PrintUnformatted(revt);
                cJSON_Delete(revt);
                if (json) {
                    size_t jl = strlen(json);
                    sctx->step_buf = (char *)AIRY_MALLOC(jl + 12);
                    if (sctx->step_buf) {
                        AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
                        AIRY_MEMCPY(sctx->step_buf + 6, json, jl);
                        sctx->step_buf[6 + jl] = '\n';
                        sctx->step_buf[6 + jl + 1] = '\n';
                        sctx->step_buf[6 + jl + 2] = '\0';
                        sctx->step_len = 6 + jl + 2;
                    }
                    AIRY_FREE(json);
                }
            }
            AIRY_FREE(sctx->reasoning);
            sctx->reasoning = NULL;
            sctx->phase = GW_SSE_PHASE_FINAL_TEXT;
            break; /* 顶部 step_buf drain 先发送 reasoning 事件 */
        }

        case GW_SSE_PHASE_FINAL_TEXT: {
            if (sctx->final_pos >= sctx->final_len) {
                /* Record the completion into the hall event flow exactly
                 * once (this branch runs a single time before [DONE]). */
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
                    /* 2.2.4 长时记忆写回：会话结束后写入 mem_d（与 CLI
                     * 相同的 "用户:/AgentRT:" 记录格式，两侧共享召回）。 */
                    gw_sse_mem_record(sctx);
                }
                /* 2.1.1.5：先透传 usage SSE 事件（若 llm_d 流式尾帧已携带），
                 * 下一次回调再发 [DONE]，保证客户端在流结束时拿到真实
                 * token 消耗与计费数据。 */
                if (sctx->usage_received && !sctx->usage_emitted) {
                    sctx->usage_emitted = 1;
                    cJSON *uevt = cJSON_CreateObject();
                    if (uevt) {
                        cJSON_AddStringToObject(uevt, "__airy_evt", "usage");
                        cJSON_AddNumberToObject(uevt, "prompt_tokens",
                                                (double)sctx->prompt_tokens);
                        cJSON_AddNumberToObject(uevt, "completion_tokens",
                                                (double)sctx->completion_tokens);
                        cJSON_AddNumberToObject(uevt, "total_tokens",
                                                (double)sctx->total_tokens);
                        cJSON_AddNumberToObject(uevt, "cost_usd", sctx->cost_usd);
                        char *json = cJSON_PrintUnformatted(uevt);
                        cJSON_Delete(uevt);
                        if (json) {
                            size_t jl = strlen(json);
                            sctx->step_buf = (char *)AIRY_MALLOC(jl + 12);
                            if (sctx->step_buf) {
                                AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
                                AIRY_MEMCPY(sctx->step_buf + 6, json, jl);
                                sctx->step_buf[6 + jl] = '\n';
                                sctx->step_buf[6 + jl + 1] = '\n';
                                sctx->step_buf[6 + jl + 2] = '\0';
                                sctx->step_len = 6 + jl + 2;
                            }
                            AIRY_FREE(json);
                        }
                    }
                    break; /* 本次回调返回 usage 帧 */
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

/* ── SSE 会话长时记忆（2.2.4 对话路径，与 CLI 对齐）────────────── */

/* 防自我回灌：跳过与当前输入几乎相同的记忆记录（CLI 把整轮写为
 * "用户: <input>\nAgentRT: <reply>"，查询常命中上一条自身）。 */
static int gw_sse_mem_is_self_feedback(const char *record_data, const char *prompt)
{
    if (!record_data || !prompt || !prompt[0])
        return 0;
    /* "用户: " 在 UTF-8 下为 9 字节（"用户"=6 字节 + 空格冒号空格=3） */
    if (strncmp(record_data, "用户: ", 9) != 0)
        return 0;
    size_t plen = strlen(prompt);
    if (strncmp(record_data + 9, prompt, plen) == 0)
        return 1;
    return 0;
}

/* 记忆注入：用本轮用户输入经 mem_d 语义检索，把相关历史记忆作为
 * system 前缀消息插到 history 首位（与 CLI cli_chat_mem_inject_system
 * 相同的语义；mem_d 不可用时静默跳过，不阻塞对话）。 */
static void gw_sse_mem_inject(cJSON *history, const char *prompt)
{
    if (!history || !prompt || !prompt[0])
        return;

    char **record_ids = NULL;
    float *scores = NULL;
    size_t count = 0;
    if (airy_sys_memory_search(prompt, 3, &record_ids, &scores, &count) != AIRY_OK ||
        count == 0) {
        goto inject_done;
    }

    char mem_acc[768];
    size_t off = 0;
    if (sizeof(mem_acc) > 0) {
        int w0 = snprintf(mem_acc + off, sizeof(mem_acc) - off, "\n\n[相关历史记忆]");
        if (w0 > 0)
            off += ((size_t)w0 < sizeof(mem_acc) - off) ? (size_t)w0 : (sizeof(mem_acc) - off - 1);
    }
    for (size_t i = 0; i < count && off < sizeof(mem_acc) - 1; i++) {
        void *data = NULL;
        size_t dlen = 0;
        if (airy_sys_memory_get(record_ids[i], &data, &dlen) != AIRY_OK || !data)
            continue;
        const char *rec = (const char *)data;
        if (!gw_sse_mem_is_self_feedback(rec, prompt)) {
            size_t n = dlen < 200 ? dlen : 200;
            int w1 = snprintf(mem_acc + off, sizeof(mem_acc) - off, "\n- %.*s", (int)n, rec);
            if (w1 > 0)
                off += ((size_t)w1 < sizeof(mem_acc) - off) ? (size_t)w1
                                                            : (sizeof(mem_acc) - off - 1);
        }
        AIRY_FREE(data);
    }
    if (off > 0 && off < sizeof(mem_acc)) {
        /* 记忆原文可能含历史会话带入的非法 UTF-8 字节，先清洗再注入，
         * 避免最终 llm 请求触发提供方 "invalid unicode" 400。 */
        char *clean = gw_sse_utf8_sanitize(mem_acc, off);
        cJSON *sys = cJSON_CreateObject();
        if (sys) {
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", clean ? clean : mem_acc);
            cJSON_InsertItemInArray(history, 0, sys);
        }
        AIRY_FREE(clean);
    }

inject_done:
    if (record_ids) {
        for (size_t i = 0; i < count; i++)
            AIRY_FREE(record_ids[i]);
        AIRY_FREE(record_ids);
    }
    if (scores)
        AIRY_FREE(scores);
}

/* JSON 字符串转义（写入元数据：reasoning 含引号/换行时需转义）。
 * 输出追加到 dst（调用方保证空间足够），返回追加长度。 */
static size_t gw_json_escape_append(char *dst, size_t cap, const char *src, size_t len)
{
    if (!dst || !src || cap == 0)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 6 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '"':
            dst[o++] = '\\';
            dst[o++] = '"';
            break;
        case '\\':
            dst[o++] = '\\';
            dst[o++] = '\\';
            break;
        case '\n':
            dst[o++] = '\\';
            dst[o++] = 'n';
            break;
        case '\r':
            dst[o++] = '\\';
            dst[o++] = 'r';
            break;
        case '\t':
            dst[o++] = '\\';
            dst[o++] = 't';
            break;
        default:
            if (c < 0x20) {
                o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
            } else {
                dst[o++] = (char)c;
            }
            break;
        }
    }
    dst[o] = '\0';
    return o;
}

/* 记忆写回：会话结束后把 "用户: <input>\nAgentRT: <reply>" 写入
 * mem_d（与 CLI 相同的记录格式，供两侧共享召回）。回复过短或
 * 内容为空不写（避免垃圾条目抬高检索噪声）。 */
static void gw_sse_mem_record(gw_sse_ctx_t *sctx)
{
    if (!sctx || sctx->mem_recorded || !sctx->user_prompt || !sctx->user_prompt[0])
        return;
    sctx->mem_recorded = 1;
    if (!sctx->final_text || strlen(sctx->final_text) < 8)
        return;

    char content[1800];
    int n = snprintf(content, sizeof(content), "用户: %s\nAgentRT: %s", sctx->user_prompt,
                     sctx->final_text);
    if (n <= 0)
        return;
    if (n > 1600)
        n = 1600;

    /* 推理链原始证据（L1）：reasoning（思考链摘要）与 token 计量随元数据
     * 落库，供记忆卷载 L1 层保留（思考输出 token 是推理链条的原始证据）。 */
    char meta[1024];
    {
        int mn = snprintf(meta, sizeof(meta),
                          "{\"source\":\"gateway\",\"kind\":\"chat\","
                          "\"prompt_tokens\":%llu,\"completion_tokens\":%llu,"
                          "\"total_tokens\":%llu,\"cost_usd\":%.6f",
                          sctx->prompt_tokens, sctx->completion_tokens, sctx->total_tokens,
                          sctx->cost_usd);
        if (sctx->reasoning && sctx->reasoning[0]) {
            /* reasoning 截断至 600 字符写入 meta（过长会撑爆元数据列） */
            size_t rl = strlen(sctx->reasoning);
            if (rl > 600)
                rl = 600;
            if (mn < (int)sizeof(meta) - 2) {
                meta[mn++] = ',';
                meta[mn++] = '"';
                const char *rk = "reasoning\":\"";
                for (const char *p = rk; *p && mn < (int)sizeof(meta) - 1; p++)
                    meta[mn++] = *p;
                size_t used = gw_json_escape_append(meta + mn, sizeof(meta) - (size_t)mn,
                                                    sctx->reasoning, rl);
                mn += (int)used;
                if (mn < (int)sizeof(meta) - 2) {
                    meta[mn++] = '"';
                }
            }
        }
        snprintf(meta + mn, sizeof(meta) - (size_t)mn, "}");
    }
    char *rid = NULL;
    airy_sys_memory_write(content, (size_t)n, meta, &rid);
    AIRY_FREE(rid);
}

/**
  * @brief MHD free_cb: release the SSE callback context (close fd + free memory)
  */
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

/**
  * @brief Handle POST /api/v1/chat/stream (SSE streaming chat with tool loop)
  *
  * Request body (one of two):
  *   1. OpenAI format: {"model":"...","messages":[{"role":"user","content":"..."}]}
  *   2. Simplified JSON-RPC agent.run: {"jsonrpc":"2.0","method":"agent.run",
  *      "params":{"prompt":"...","model":"...","messages":[...]}}
  * messages may be empty; then build [{"role":"user","content":prompt}] from prompt;
  * Return 400 if both are missing.
  *
  * Flow: parse model/messages -> seed the tool-loop state machine (messages
  * kept in cJSON form) -> stream via MHD_create_response_from_callback.
  * The content_reader drives llm.complete_stream rounds (with the full
  * tool schema): text chunks are forwarded immediately as SSE events (real
  * streaming), tool_calls are executed through tool.sock with results fed
  * back, and the reply is terminated by [DONE].
  */
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

    /* 1.2.6 修复：请求体可能携带非法 UTF-8（粘贴/记忆召回中的脏字节），
     * DeepSeek 等提供方会直接 400 "invalid unicode code point" 导致前端
     * 无回复。解析前清洗整份 JSON 文本（无效字节 → U+FFFD）；JSON 结构
     * 字符均为 ASCII，清洗不影响语法。 */
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

    /* Seed the tool-loop conversation history: duplicate messages, or build a
     * single user message from prompt. */
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
    /* model may point into `root`; duplicate it before root is freed so the
     * session model string never dangles after cJSON_Delete(root). */
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

    /* 2.2.4 长时记忆：保存本轮首个用户输入（记忆写回键），并注入
     * 相关历史记忆为 system 前缀（与 CLI 对话路径同语义——TUI 对话
     * 获得长时记忆，CLI/TUI 切换后共享同一记忆库召回上一句上下文；
     * mem_d 不可用时注入静默跳过，不影响对话）。 */
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

    /* Record the session start into the hall event flow (SSoT write side):
     * every gateway chat session becomes visible on the board / decision
     * chain / event stream, keeping the "seen == recorded" invariant. */
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

/* ── hall.watch SSE push (read side of the event flow) ───────────────
 *
 * Long-lived subscription: each content_reader invocation scans the hall
 * root on disk, and when a new event was recorded since the last call it
 * is forwarded as an SSE data frame. This turns the single-source-of-truth
 * event flow into a push stream (hall.stream is poll-based pull; watch is
 * the real-time push counterpart). When no new events exist the callback
 * sleeps briefly and returns a keep-alive comment to hold the connection
 * open without busy-looping the event loop. */

typedef struct {
    gw_hall_watch_t watch;
    int done;
} gw_hall_watch_ctx_t;

static ssize_t gw_hall_watch_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    gw_hall_watch_ctx_t *w = (gw_hall_watch_ctx_t *)cls;
    if (!w || w->done)
        return MHD_CONTENT_READER_END_OF_STREAM;
    if (max < 8)
        return MHD_CONTENT_READER_END_OF_STREAM;

    char evt[8192];
    int r = gw_hall_watch_next(&w->watch, evt, sizeof(evt));
    if (r < 0) {
        w->done = 1;
        return MHD_CONTENT_READER_END_OF_STREAM;
    }
    if (r > 0) {
        /* SSE data frame: data: <compact event JSON>\n\n */
        size_t el = strlen(evt);
        if (el + 8 > max) {
            el = max - 8;
            evt[el] = '\0';
        }
        size_t n = 0;
        AIRY_MEMCPY(buf + n, "data: ", 6);
        n += 6;
        AIRY_MEMCPY(buf + n, evt, el);
        n += el;
        buf[n++] = '\n';
        buf[n++] = '\n';
        return (ssize_t)n;
    }

    /* No new events: hold the connection with a keep-alive comment. */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
    nanosleep(&ts, NULL);
    AIRY_MEMCPY(buf, ": keep-alive\n\n", 15);
    return 15;
}

static void gw_hall_watch_free(void *cls)
{
    gw_hall_watch_ctx_t *w = (gw_hall_watch_ctx_t *)cls;
    AIRY_FREE(w);
}

/**
  * @brief Handle GET /api/v1/hall/watch (SSE hall event push)
  *
  * Long-lived SSE subscription over the hall event flow. Every newly
  * recorded hall event (progress / result / blueprint / chain ... from any
  * writer process) is pushed to the client as an SSE data frame, in global
  * (ts_utc, seq) order. This closes the gap where hall events were only
  * written (recorded) but never pushed to live subscribers: hall.stream is
  * a poll-based pull, hall.watch is the real-time push.
  */
int handle_hall_watch_sse(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context)
{
    (void)context;
    gw_hall_watch_ctx_t *w = (gw_hall_watch_ctx_t *)AIRY_CALLOC(1, sizeof(*w));
    if (!w)
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    gw_hall_watch_init(&w->watch);

    struct MHD_Response *response =
        MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, GW_SSE_BLOCK_SIZE,
                                          gw_hall_watch_reader, w, gw_hall_watch_free);
    if (!response) {
        AIRY_FREE(w);
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
}
