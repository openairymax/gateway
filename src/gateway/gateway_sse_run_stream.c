// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gateway_sse_run_stream.c
 * @brief agent.run_stream SSE 纯翻译端点（M1-1d 协议先行）。
 *
 * 依据 0.1.9 架构改进方案 §2.4.5：agent_d 引擎以流式事件帧（单行 JSON，
 * \n 收尾）向 gateway 推送，gateway 仅做帧封装翻译为 SSE data: 帧，
 * 零业务逻辑（K-1 纯翻译）。
 *
 * 端点：POST /api/v1/agent/run/stream
 *   - 请求体：JSON-RPC 2.0（method=agent.run_stream，params 透传）
 *   - 响应：200 + text/event-stream；每事件一个 "data: <json>\n\n" 帧
 *   - 中断：客户端断开后关闭到 agent_d 的连接，流结束
 */

#include "http_gateway_sse_internal.h"

#include "airy_memory.h"
#include "airy_run_stream.h"
#include "logging.h"
#include "platform.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32

#define RS_SSE_BUF_CAP (64 * 1024)

/* run_stream SSE 翻译上下文（仅帧缓冲与 agent_d socket，无业务状态） */
typedef struct {
    int fd;
    int done;
    char *buf;
    size_t len;
    size_t cap;
    int eof;
} rs_sse_ctx_t;

/* 解析 agent.sock 路径（env 覆盖 -> $AIRY_RUNTIME_DIR/agent.sock） */
static void rs_resolve_agent_sock(char *out, size_t out_size)
{
    const char *env = getenv("AIRY_AGENT_SOCK");
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_size);
        return;
    }
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir) {
        snprintf(out, out_size, "%s/agent.sock", run_dir);
    } else {
        AIRY_STRNCPY_TERM(out, "agent.sock", out_size);
    }
}

/* 构造 agent.run_stream JSON-RPC 请求（params 原样透传） */
static char *rs_build_request(const cJSON *params)
{
    cJSON *req = cJSON_CreateObject();
    if (!req)
        return NULL;
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", "agent.run_stream");
    if (cJSON_IsObject(params)) {
        cJSON_AddItemToObject(req, "params", cJSON_Duplicate(params, 1));
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return req_str;
}

/* 连接 agent.sock 并发送 run_stream 请求；返回 fd（-1 失败） */
static int rs_connect(const char *sock_path, const char *req_json)
{
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
}

/* 追加一块数据到行缓冲（超出上限截断丢弃，防无限膨胀） */
static int rs_append(rs_sse_ctx_t *rs, const char *data, size_t n)
{
    if (n == 0)
        return 0;
    if (rs->len + n + 1 > rs->cap) {
        size_t new_cap = rs->cap ? rs->cap : 8192;
        while (new_cap < rs->len + n + 1)
            new_cap *= 2;
        if (new_cap > RS_SSE_BUF_CAP)
            return 0;
        char *np = (char *)AIRY_REALLOC(rs->buf, new_cap);
        if (!np)
            return 0;
        rs->buf = np;
        rs->cap = new_cap;
    }
    AIRY_MEMCPY(rs->buf + rs->len, data, n);
    rs->len += n;
    rs->buf[rs->len] = '\0';
    return 1;
}

/* 从缓冲中取出完整一行（\n 收尾），翻译为 SSE data: 帧；无完整行返回 NULL */
static char *rs_take_line(rs_sse_ctx_t *rs)
{
    char *nl = rs->buf ? (char *)memchr(rs->buf, '\n', rs->len) : NULL;
    if (!nl)
        return NULL;
    size_t line_len = (size_t)(nl - rs->buf);
    /* 跳过空行 */
    if (line_len == 0) {
        rs->len -= 1;
        AIRY_MEMMOVE(rs->buf, nl + 1, rs->len);
        rs->buf[rs->len] = '\0';
        return rs_take_line(rs);
    }
    size_t frame_len = line_len + 8; /* "data: " + line + "\n\n" */
    char *frame = (char *)AIRY_MALLOC(frame_len + 1);
    if (!frame)
        return NULL;
    AIRY_MEMCPY(frame, "data: ", 6);
    AIRY_MEMCPY(frame + 6, rs->buf, line_len);
    frame[6 + line_len] = '\n';
    frame[6 + line_len + 1] = '\n';
    frame[6 + line_len + 2] = '\0';
    rs->len -= line_len + 1;
    AIRY_MEMMOVE(rs->buf, nl + 1, rs->len);
    rs->buf[rs->len] = '\0';
    return frame;
}

/* MHD content reader：从 agent_d socket 拉事件帧并翻译为 SSE data: 帧 */
static ssize_t rs_content_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    rs_sse_ctx_t *rs = (rs_sse_ctx_t *)cls;
    if (!rs || rs->done)
        return MHD_CONTENT_READER_END_OF_STREAM;
    if (max < 8)
        return MHD_CONTENT_READER_END_OF_STREAM;

    for (;;) {
        char *frame = rs_take_line(rs);
        if (frame) {
            size_t fl = strlen(frame);
            if (fl <= max) {
                AIRY_MEMCPY(buf, frame, fl);
                AIRY_FREE(frame);
                return (ssize_t)fl;
            }
            AIRY_FREE(frame);
            return MHD_CONTENT_READER_END_OF_STREAM;
        }
        if (rs->eof) {
            rs->done = 1;
            return MHD_CONTENT_READER_END_OF_STREAM;
        }
        char tmp[4096];
        ssize_t n = recv(rs->fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            if (!rs_append(rs, tmp, (size_t)n))
                return MHD_CONTENT_READER_END_OF_STREAM;
            continue;
        }
        if (n == 0) {
            rs->eof = 1;
            close(rs->fd);
            rs->fd = -1;
            /* 排空缓冲中剩余无 \n 尾巴的一行 */
            char *frame = rs_take_line(rs);
            if (frame) {
                size_t fl = strlen(frame);
                if (fl <= max) {
                    AIRY_MEMCPY(buf, frame, fl);
                    AIRY_FREE(frame);
                    return (ssize_t)fl;
                }
                AIRY_FREE(frame);
            }
            rs->done = 1;
            return MHD_CONTENT_READER_END_OF_STREAM;
        }
        if (errno == EINTR)
            continue;
        /* EAGAIN 等：短暂让出后重试（保持流存活） */
        return 0;
    }
}

static void rs_content_free(void *cls)
{
    rs_sse_ctx_t *rs = (rs_sse_ctx_t *)cls;
    if (!rs)
        return;
    if (rs->fd >= 0)
        close(rs->fd);
    AIRY_FREE(rs->buf);
    AIRY_FREE(rs);
}

/* POST /api/v1/agent/run/stream */
int handle_run_stream_sse(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context)
{
    const char *body = context->body_buf;
    size_t body_len = context->body_len;
    if (!body || body_len == 0) {
        return gw_sse_send_json_error(gateway, connection, 400,
                                      "Request body required (JSON-RPC agent.run_stream)");
    }

    char *body_copy = (char *)AIRY_MALLOC(body_len + 1);
    if (!body_copy) {
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    AIRY_MEMCPY(body_copy, body, body_len);
    body_copy[body_len] = '\0';

    cJSON *root = cJSON_Parse(body_copy);
    AIRY_FREE(body_copy);
    if (!root) {
        return gw_sse_send_json_error(gateway, connection, 400, "Invalid JSON body");
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    char *req_json = rs_build_request(cJSON_IsObject(params) ? params : NULL);
    cJSON_Delete(root);
    if (!req_json) {
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }

    char agent_sock[256];
    rs_resolve_agent_sock(agent_sock, sizeof(agent_sock));
    int fd = rs_connect(agent_sock, req_json);
    AIRY_FREE(req_json);
    if (fd < 0) {
        AIRY_LOG_WARN("run_stream: agent.sock connect failed (%s)", agent_sock);
        return gw_sse_send_json_error(gateway, connection, 503, "Agent service unreachable");
    }

    rs_sse_ctx_t *rs = (rs_sse_ctx_t *)AIRY_CALLOC(1, sizeof(rs_sse_ctx_t));
    if (!rs) {
        close(fd);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    rs->fd = fd;

    struct MHD_Response *response =
        MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 4096, rs_content_reader, rs,
                                          rs_content_free);
    if (!response) {
        rs_content_free(rs);
        return gw_sse_send_json_error(gateway, connection, 500, "Failed to create stream response");
    }
    MHD_add_response_header(response, "Content-Type", "text/event-stream");
    MHD_add_response_header(response, "Cache-Control", "no-cache");
    MHD_add_response_header(response, "Connection", "keep-alive");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    gateway_apply_cors_headers(gateway, connection, response);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    return ret;
}

#endif /* !_WIN32 */
