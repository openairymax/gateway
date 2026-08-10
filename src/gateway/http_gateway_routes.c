/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file http_gateway_routes.c
 * @brief HTTP 网关路由处理函数实现
 *
 * 将 handle_http_request 的复杂逻辑拆分为独立的路由处理函数，
 * 降低圈复杂度，提高可维护性。
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

// @owner: team-B
#include "http_gateway_routes.h"

#include "gateway_rate_limiter.h"
#include "gateway_rpc_handler.h"
#include "gateway_utils.h"
#include "http_gateway.h"
#include "jsonrpc.h"
#include "logging.h"
#include "airy_memory.h"
#include "platform.h" /* airy_runtime_dir()：SSE 端点解析 llm_d socket 路径 */
#include "syscall_router.h"
#include "syscalls.h"

#include <microhttpd.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif
#include <stdlib.h>
#include <string.h>

/* MHD header iterator callback (same as http_gateway.c) */
static int parse_headers(void *cls __attribute__((unused)),
                         enum MHD_ValueKind kind __attribute__((unused)),
                         const char *key __attribute__((unused)),
                         const char *value __attribute__((unused)))
{
    return MHD_YES;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h> /* SO_RCVTIMEO 用 struct timeval */
#include <sys/un.h>   /* SSE 端点连接 llm_d Unix socket */
#include <unistd.h>   /* close() */
#endif

/* 跨平台原子操作支持 - 使用统一的 atomic_compat.h */
#include "atomic_compat.h"

/* ========== 路由处理函数实现 ========== */

/**
 * @brief 处理 JSON-RPC POST 请求 (CC=3)
 */
int handle_post_jsonrpc(http_gateway_t *gateway,
                        struct MHD_Connection *connection,
                        http_request_context_t *context)
{

    char *json_response = handle_jsonrpc_request(gateway, context);
    if (!json_response) {
        const char *err_msg = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":"
                              "\"Internal error\"},\"id\":null}";
        struct MHD_Response *response = create_http_response_ex(gateway, connection, 500, err_msg, strlen(err_msg));
        int ret = MHD_queue_response(connection, 500, response);
        MHD_destroy_response(response);
        return ret;
    }
    struct MHD_Response *response = create_http_response_ex(gateway, connection, 200, json_response, strlen(json_response));

    uint64_t response_time_ns = gateway_time_ns() - context->start_time_ns;
    LOG_DEBUG("请求处理耗时: %lu ns", response_time_ns);

    atomic_fetch_add(&gateway->requests_total, 1);
    atomic_fetch_add(&gateway->bytes_received, context->upload_data_size);
    atomic_fetch_add(&gateway->bytes_sent, strlen(json_response));

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    AIRY_FREE(json_response);
    return ret;
}

/**
 * @brief 处理 OPTIONS 请求（CORS 预检）(CC=2)
 */
int handle_options_preflight(http_gateway_t *gateway,
                             struct MHD_Connection *connection,
                             http_request_context_t *context __attribute__((unused)))
{

    struct MHD_Response *response =
        MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);

    gateway_apply_security_headers(response);
    gateway_apply_cors_headers(gateway, connection, response);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * @brief 验证API密钥（用于敏感端点保护）
 * @param connection MHD连接对象
 * @param gateway 网关实例
 * @return true 验证通过，false 拒绝访问
 */
static bool gateway_verify_api_key(struct MHD_Connection *connection,
                                   http_gateway_t *gateway __attribute__((unused)))
{

    const char *env_key = getenv("GATEWAY_API_KEY");
    if (!env_key || !env_key[0])
        return false;

    const char *auth_header =
        MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
    if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0) {
        if (strcmp(auth_header + 7, env_key) == 0)
            return true;
    }

    const char *key_param =
        MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "api_key");
    if (key_param && strcmp(key_param, env_key) == 0)
        return true;

    return false;
}

/**
 * @brief URL路径安全净化
 * @param url 原始URL路径
 * @return true 路径安全，false 检测到可疑模式
 */
static bool gateway_is_url_safe(const char *url)
{
    if (!url || !url[0])
        return false;

    size_t len = strlen(url);
    if (len > 2048)
        return false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c < 0x20 || c > 0x7E)
            return false;
    }

    if (strstr(url, "..") != NULL)
        return false;
    if (strstr(url, "%2e") != NULL || strstr(url, "%2E") != NULL)
        return false;
    if (strstr(url, "%3b") != NULL || strstr(url, "%3B") != NULL)
        return false;
    if (strstr(url, "%00") != NULL)
        return false;

    return true;
}

/**
 * @brief 处理 GET /health 健康检查 (CC=2)
 */
int handle_health_check(http_gateway_t *gateway, struct MHD_Connection *connection,
                        http_request_context_t *context __attribute__((unused)))
{

    const char *health_json = "{\"status\":\"healthy\",\"service\":\"gateway\"}";
    struct MHD_Response *response = create_http_response_ex(gateway, connection, 200, health_json, strlen(health_json));

    atomic_fetch_add(&gateway->requests_total, 1);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * @brief 处理 GET /metrics 指标导出 (CC=3) — 需要API密钥认证
 */
int handle_metrics_export(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context __attribute__((unused)))
{

    if (!gateway_verify_api_key(connection, gateway)) {
        const char *err_json =
            "{\"error\":{\"code\":-32001,\"message\":\"Unauthorized: API key required\"}}";
        struct MHD_Response *response = create_http_response_ex(gateway, connection, 401, err_json, strlen(err_json));
        int ret = MHD_queue_response(connection, 401, response);
        MHD_destroy_response(response);
        atomic_fetch_add(&gateway->requests_failed, 1);
        return ret;
    }

    char *metrics_json = NULL;
    airy_err_t err = airy_sys_telemetry_metrics(&metrics_json);

    if (err != AIRY_SUCCESS || !metrics_json) {
        metrics_json = AIRY_STRDUP("{\"error\":\"failed to get metrics\"}");
    }

    struct MHD_Response *response = create_http_response_ex(gateway, connection, 200, metrics_json, strlen(metrics_json));
    AIRY_FREE(metrics_json);

    atomic_fetch_add(&gateway->requests_total, 1);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * @brief 处理 404 Not Found (CC=2)
 */
int handle_not_found(http_gateway_t *gateway,
                     struct MHD_Connection *connection, http_request_context_t *context)
{

    char *error_response = jsonrpc_create_error_response(NULL, -32601, "Not Found", NULL);
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 404, error_response, strlen(error_response));
    AIRY_FREE(error_response);

    atomic_fetch_add(&gateway->requests_failed, 1);

    int ret = MHD_queue_response(connection, 404, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * @brief 处理请求大小超限错误 (CC=2)
 */
int handle_request_too_large(http_gateway_t *gateway, struct MHD_Connection *connection,
                             http_request_context_t *context __attribute__((unused)),
                             size_t data_size)
{

    char *error_response = jsonrpc_create_error_response(NULL, -413, "Request too large", NULL);
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 413, error_response, strlen(error_response));
    AIRY_FREE(error_response);

    atomic_fetch_add(&gateway->requests_failed, 1);
    atomic_fetch_add(&gateway->bytes_received, data_size);

    int ret = MHD_queue_response(connection, 413, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * @brief 处理 JSON 解析错误 (CC=2)
 */
int handle_parse_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                       http_request_context_t *context __attribute__((unused)), size_t data_size)
{

    char *error_response = jsonrpc_create_error_response(NULL, -32700, "Parse error", NULL);
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 400, error_response, strlen(error_response));
    AIRY_FREE(error_response);

    atomic_fetch_add(&gateway->requests_failed, 1);
    atomic_fetch_add(&gateway->bytes_received, data_size);

    int ret = MHD_queue_response(connection, 400, response);
    MHD_destroy_response(response);

    return ret;
}

/* ========== SSE 流式聊天端点（POST /api/v1/chat/stream） ========== */

/**
 * @brief SSE 流式聊天端点常量
 *
 * 端点语义：网关作为 llm_d complete_stream 的流式转发代理。客户端 POST
 * OpenAI messages 格式（或 JSON-RPC agent.run 简化格式），网关直连 llm_d
 * 拉取增量文本块，以 SSE 事件逐块转发，供 TUI 增量渲染对话。
 * 设计决定：仅 LLM 直连流式，无工具循环/无 think_d（Claude Code 风格对话流式）。
 */
#define GW_SSE_CHAT_PATH "/api/v1/chat/stream"
#define GW_SSE_DEFAULT_MODEL "deepseek-v4-flash" /* 与 llm_d model.yaml global.default_model 对齐 */
#define GW_SSE_RECV_TIMEOUT_S 30                /* llm_d 增量块 recv 超时 */
#define GW_SSE_BLOCK_SIZE 1024                  /* MHD content_reader 单轮输出上限 */
#define GW_SSE_DONE_EVENT "data: [DONE]\n\n"    /* 流结束事件（14 字节） */

/**
 * @brief SSE 流式响应回调上下文
 *
 * MHD_create_response_from_callback 的 cls：持有 llm_d socket fd 与结束标志。
 * 由 MHD 的 free_cb（gw_sse_content_free）统一释放（关闭 fd + AIRY_FREE）。
 */
typedef struct {
    int fd;   /**< llm_d Unix socket fd（-1 表示无效） */
    int done; /**< 已输出终止事件（[DONE]），下次回调返回 MHD_CONTENT_READER_END_OF_STREAM */
} gw_sse_ctx_t;

/**
 * @brief 解析 llm_d socket 路径：env AIRY_LLM_SOCK → airy_runtime_dir()/llm.sock
 *
 * 与 gateway_business_handler.c 的 gw_resolve_daemon_sock 同源：
 * airy_runtime_dir() 解析 $AIRY_HOME/run，缺省 ~/.airymaxrt/run。
 * （airy_runtime_dir_socket 使用静态缓冲仅限启动期单线程，故此处自行拼接。）
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
 * @brief 发送 JSON 错误响应（SSE 端点非流式失败路径：400/500/502）
 */
static int gw_sse_send_json_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                                  int status, const char *message)
{
    char err[256];
    int n = snprintf(err, sizeof(err), "{\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                     status, message ? message : "error");
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
 * @brief MHD content_reader：从 llm_d socket 拉取增量块并包装为 SSE 事件
 *
 * MHD 语义（microhttpd.h）：返回值 >0 为写入 buf 的字节数；
 * MHD_CONTENT_READER_END_OF_STREAM (-1) 表示流结束（size=MHD_SIZE_UNKNOWN +
 * chunked 编码下 MHD 结束 chunk 并完成传输）。size=MHD_SIZE_UNKNOWN 时
 * pos 为已输出累计长度（本实现不依赖）。
 *
 * 每轮输出格式：`data: <块内容>\n\n`（块内容按字节转发，逐块）。
 * recv 返回 0（llm_d 连接关闭）或超时/错误时输出 `data: [DONE]\n\n`
 * 并置 done，下一轮回调返回 MHD_CONTENT_READER_END_OF_STREAM 终止。
 */
static ssize_t gw_sse_content_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)cls;
    if (!sctx || sctx->fd < 0 || sctx->done)
        return MHD_CONTENT_READER_END_OF_STREAM;
    /* 防御：buf 需容纳 "data: "(6) + 数据 + "\n\n"(2)；
     * block_size=1024 下 max>=1024，此分支不会触发 */
    if (max < sizeof(GW_SSE_DONE_EVENT))
        return MHD_CONTENT_READER_END_OF_STREAM;

    /* 预留 SSE 包装开销（6+2），最大可 recv 的净荷 */
    size_t want = max - 8;
    ssize_t n;
    do {
        n = recv(sctx->fd, buf + 6, want, 0);
    } while (n < 0 && errno == EINTR);
    if (n > 0) {
        AIRY_MEMCPY(buf, "data: ", 6);
        buf[6 + n] = '\n';
        buf[6 + n + 1] = '\n';
        return (ssize_t)(6 + n + 2);
    }
    /* EOF（llm_d 关闭连接）或超时/错误：输出终止事件并结束 */
    sctx->done = 1;
    AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
    return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
}

/**
 * @brief MHD free_cb：释放 SSE 流式响应回调上下文（关闭 fd + 释放内存）
 */
static void gw_sse_content_free(void *cls)
{
    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)cls;
    if (!sctx)
        return;
    if (sctx->fd >= 0)
        close(sctx->fd);
    AIRY_FREE(sctx);
}

/**
 * @brief 处理 POST /api/v1/chat/stream（SSE 流式聊天，CC=5）
 *
 * 请求体（二选一）：
 *   1. OpenAI 格式：{"model":"...","messages":[{"role":"user","content":"..."}]}
 *   2. JSON-RPC agent.run 简化：{"jsonrpc":"2.0","method":"agent.run",
 *      "params":{"prompt":"...","model":"...","messages":[...]}}
 * messages 可为空数组，此时用 prompt 构造 [{"role":"user","content":prompt}]；
 * 两者均缺省返回 400。
 *
 * 处理流程：解析 model/messages → 构造 complete_stream JSON-RPC 请求 →
 * 连接 llm_d（AIRY_LLM_SOCK → $AIRY_RUNTIME_DIR/llm.sock，SO_RCVTIMEO 30s）
 * → 发送请求 → MHD_create_response_from_callback 流式转发（content_reader
 * 逐块 recv 并包装为 SSE 事件，EOF 输出 [DONE]）。llm_d 不可达时返回 502。
 */
int handle_chat_stream_sse(http_gateway_t *gateway, struct MHD_Connection *connection,
                           http_request_context_t *context)
{
#ifndef _WIN32
    const char *body = context->upload_data;
    size_t body_len = context->upload_data_size;
    if (!body || body_len == 0) {
        return gw_sse_send_json_error(gateway, connection, 400,
                                      "Request body required (model+messages or prompt)");
    }

    /* 请求体拷贝为 NUL 结尾（MHD upload_data 不保证以 '\0' 结尾） */
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

    /* 解析 model / messages / prompt（JSON-RPC agent.run 格式参数在 params 内） */
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

    /* 构造 complete_stream 请求参数：model + messages（缺省用 prompt 构造 user 消息） */
    cJSON *llm_params = cJSON_CreateObject();
    if (!llm_params) {
        cJSON_Delete(root);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    cJSON_AddStringToObject(llm_params, "model", model);
    if (messages && cJSON_GetArraySize(messages) > 0) {
        cJSON *dup = cJSON_Duplicate(messages, 1);
        if (!dup) {
            cJSON_Delete(llm_params);
            cJSON_Delete(root);
            return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
        }
        cJSON_AddItemToObject(llm_params, "messages", dup);
    } else if (prompt) {
        cJSON *arr = cJSON_CreateArray();
        cJSON *msg = cJSON_CreateObject();
        if (!arr || !msg) {
            if (msg)
                cJSON_Delete(msg);
            if (arr)
                cJSON_Delete(arr);
            cJSON_Delete(llm_params);
            cJSON_Delete(root);
            return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
        }
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", prompt->valuestring);
        cJSON_AddItemToArray(arr, msg);
        cJSON_AddItemToObject(llm_params, "messages", arr);
    } else {
        cJSON_Delete(llm_params);
        cJSON_Delete(root);
        return gw_sse_send_json_error(gateway, connection, 400,
                                      "messages or prompt required");
    }
    cJSON_Delete(root);

    /* 构造完整 JSON-RPC complete_stream 请求 */
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        cJSON_Delete(llm_params);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", "complete_stream");
    cJSON_AddItemToObject(req, "params", llm_params); /* 所有权转移给 req */
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }

    /* 连接 llm_d（POSIX Unix socket） */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        AIRY_FREE(req_str);
        return gw_sse_send_json_error(gateway, connection, 502, "LLM service unreachable");
    }
    char sock_path[256];
    gw_sse_resolve_llm_sock(sock_path, sizeof(sock_path));
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN("gateway sse: cannot connect to llm_d (sock=%s)", sock_path);
        close(fd);
        AIRY_FREE(req_str);
        return gw_sse_send_json_error(gateway, connection, 502, "LLM service unreachable");
    }

    /* 接收超时：LLM 首个增量块可能需数秒思考时间 */
    struct timeval tv = {GW_SSE_RECV_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 发送完整 JSON-RPC complete_stream 请求 */
    size_t len = strlen(req_str);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_str + sent, len - sent, 0);
        if (n <= 0) {
            LOG_WARN("gateway sse: failed to send complete_stream to llm_d");
            close(fd);
            AIRY_FREE(req_str);
            return gw_sse_send_json_error(gateway, connection, 502, "LLM service unreachable");
        }
        sent += (size_t)n;
    }
    AIRY_FREE(req_str);

    /* 创建流式响应（size=MHD_SIZE_UNKNOWN → chunked 编码；content_reader 逐块转发） */
    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)AIRY_CALLOC(1, sizeof(gw_sse_ctx_t));
    if (!sctx) {
        close(fd);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    sctx->fd = fd;
    sctx->done = 0;

    struct MHD_Response *response = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, GW_SSE_BLOCK_SIZE, gw_sse_content_reader, sctx, gw_sse_content_free);
    if (!response) {
        close(fd);
        AIRY_FREE(sctx);
        return gw_sse_send_json_error(gateway, connection, 500,
                                      "Failed to create stream response");
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
    return gw_sse_send_json_error(gateway, connection, 501, "SSE streaming unsupported on this platform");
#endif
}

/* ========== 路由表定义（唯一实现） ========== */

/**
 * @brief HTTP 路由表（按优先级排序）
 *
 * 路由匹配规则：
 * 1. 先匹配 HTTP 方法
 * 2. 再匹配路径（支持通配符 "*"）
 * 3. 未匹配则走默认路由 (handle_not_found)
 */
static const http_route_t http_routes[] = {
    {"POST", "/", handle_post_jsonrpc},
    {"POST", "/api/v1/chat/stream", handle_chat_stream_sse},
    {"OPTIONS", "*", handle_options_preflight},
    {"GET", "/health", handle_health_check},
    {"GET", "/metrics", handle_metrics_export},
    {NULL, NULL, handle_not_found} /* 默认路由（必须最后） */
};

/**
 * @brief 查找匹配的路由处理函数 (CC=2)
 *
 * @param method HTTP 方法（如 "POST", "GET"）
 * @param path URL 路径（如 "/", "/health"）
 * @return 匹配的路由处理函数，未匹配返回 NULL
 */
static http_route_handler_t find_http_route(const char *method, const char *path)
{
    for (const http_route_t *route = http_routes; route->method != NULL; route++) {
        if (strcmp(method, route->method) == 0) {
            if (strcmp(route->path, "*") == 0 || strcmp(path, route->path) == 0) {
                return route->handler;
            }
        }
    }
    return NULL;
}

/**
 * @brief 搜索并处理动态注册的端点 (CC=4)
 *
 * 将 MHD 请求/响应桥接到 gateway_endpoint_request_t / gateway_endpoint_response_t，
 * 调用用户注册的 handler，再将响应桥接回 MHD。
 *
 * @param gateway HTTP网关实例
 * @param connection MHD连接对象
 * @param context 请求上下文
 * @param method HTTP方法
 * @param url 请求URL
 * @return MHD_YES/MHD_NO
 */
static int handle_dynamic_endpoint_route(http_gateway_t *gateway, struct MHD_Connection *connection,
                                         http_request_context_t *context, const char *method,
                                         const char *url)
{
    const http_dynamic_endpoint_t *matched = NULL;

    for (size_t i = 0; i < gateway->dynamic_endpoint_count; i++) {
        const http_dynamic_endpoint_t *ep = &gateway->dynamic_endpoints[i];
        if (strcmp(method, ep->method) == 0 && strcmp(url, ep->path) == 0) {
            matched = ep;
            break;
        }
    }

    if (!matched) {
        return MHD_NO;
    }

    gateway_endpoint_request_t req = {.method = method,
                                      .path = url,
                                      .body = context->upload_data,
                                      .body_len = context->upload_data_size,
                                      .user_data = matched->user_data};

    gateway_endpoint_response_t resp = {
        .status_code = 500, .content_type = "application/json", .body = NULL, .body_len = 0};

    int handler_ret = matched->handler(&req, &resp);

    struct MHD_Response *response = NULL;
    int ret = MHD_NO;

    if (handler_ret == 0 && resp.body) {
        response = MHD_create_response_from_buffer(resp.body_len, (void *)resp.body,
                                                   MHD_RESPMEM_MUST_COPY);
        if (response) {
            MHD_add_response_header(response, "Content-Type", resp.content_type);
            gateway_apply_security_headers(response);
            gateway_apply_cors_headers(gateway, connection, response);
            ret = MHD_queue_response(connection, resp.status_code, response);
            MHD_destroy_response(response);
        }
        atomic_fetch_add(&gateway->requests_total, 1);
        atomic_fetch_add(&gateway->bytes_sent, resp.body_len);
    } else {
        const char *err_body = "{\"error\":\"Internal server error\"}";
        response = MHD_create_response_from_buffer(strlen(err_body), (void *)err_body,
                                                   MHD_RESPMEM_PERSISTENT);
        if (response) {
            MHD_add_response_header(response, "Content-Type", "application/json");
            gateway_apply_security_headers(response);
            gateway_apply_cors_headers(gateway, connection, response);
            ret = MHD_queue_response(connection, 500, response);
            MHD_destroy_response(response);
        }
        atomic_fetch_add(&gateway->requests_failed, 1);
    }

    AIRY_FREE(resp.body);

    return ret;
}

/* ========== 重构后的主请求处理函数 (CC=8) ========== */

/**
 * @brief HTTP 请求处理主函数
 *
 * 处理流程（4个阶段）：
 * 阶段1: 初始化请求上下文（首次调用）
 * 阶段2: 接收 POST 数据体
 * 阶段3: 处理完整 JSON-RPC 请求
 * 阶段4: 路由到其他端点（OPTIONS/GET等）
 */
int handle_http_request(void *cls, struct MHD_Connection *connection, const char *url,
                        const char *method, const char *version __attribute__((unused)),
                        const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    http_gateway_t *gateway = (http_gateway_t *)cls;
    http_request_context_t *context = (http_request_context_t *)*con_cls;

    /* 速率限制检查（在早期阶段进行） */
    if (gateway->rate_limiter) {
        const char *client_ip =
            MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Forwarded-For");
        if (!client_ip) {
            client_ip = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Real-IP");
        }
        if (!client_ip) {
            const union MHD_ConnectionInfo *cinfo =
                MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
            const struct sockaddr *addr =
                cinfo ? (const struct sockaddr *)cinfo->client_addr : NULL;
            if (addr) {
                char ip_buf[64];
                if (addr->sa_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in *)addr)->sin_addr, ip_buf,
                              sizeof(ip_buf));
                    client_ip = ip_buf;
                } else if (addr->sa_family == AF_INET6) {
                    inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, ip_buf,
                              sizeof(ip_buf));
                    client_ip = ip_buf;
                }
            }
        }
        if (!client_ip) {
            client_ip = "_unresolved";
        }

        if (!gateway_rate_limiter_allow(gateway->rate_limiter, client_ip)) {
            /* 返回 429 Too Many Requests */
            const char *error_response =
                "{\"error\":{\"code\":-32004,\"message\":\"Rate limit exceeded\"}}";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(error_response), (void *)error_response, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Content-Type", "application/json");
            MHD_add_response_header(response, "Server", "AgentRT-gateway/1.0");
            gateway_apply_security_headers(response);
            gateway_apply_cors_headers(gateway, connection, response);
            int ret = MHD_queue_response(connection, 429, response);
            MHD_destroy_response(response);
            return ret;
        }
    }

    /* 阶段 1: 初始化请求上下文 */
    if (!context) {
        context = AIRY_CALLOC(1, sizeof(http_request_context_t));
        if (!context) {
            return MHD_NO;
        }

        if (!gateway_is_url_safe(url)) {
            AIRY_FREE(context);
            const char *error_response =
                "{\"error\":{\"code\":-32002,\"message\":\"Invalid URL path\"}}";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(error_response), (void *)error_response, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Content-Type", "application/json");
            gateway_apply_security_headers(response);
            gateway_apply_cors_headers(gateway, connection, response);
            int ret = MHD_queue_response(connection, 400, response);
            MHD_destroy_response(response);
            return ret;
        }

        context->method = method;
        context->url = url;
        context->start_time_ns = gateway_time_ns();
        *con_cls = context;

        MHD_get_connection_values(connection, MHD_HEADER_KIND, (MHD_KeyValueIterator)parse_headers,
                                  context);

        return MHD_YES;
    }

    /* 阶段 2: 处理 POST 数据 */
    if (strcmp(method, "POST") == 0 && upload_data && *upload_data_size > 0) {
        if (*upload_data_size > gateway->max_request_size) {
            return handle_request_too_large(gateway, connection, context, *upload_data_size);
        }

        context->upload_data = upload_data;
        context->upload_data_size = *upload_data_size;

        if (parse_json_request(gateway, context, upload_data, *upload_data_size) != 0) {
            return handle_parse_error(gateway, connection, context, *upload_data_size);
        }

        *upload_data_size = 0;
        return MHD_YES;
    }

    /* 阶段 3: 处理完整请求（路由分发）——JSON-RPC 或非 JSON-RPC 原始 body 均处理。
     * SSE 流式端点（POST /api/v1/chat/stream）除外：响应为持续 SSE 事件流，
     * 必须由静态路由表直接处理（handle_chat_stream_sse），不能走一次性 JSON 响应。 */
    if (strcmp(method, "POST") == 0 && strcmp(url, GW_SSE_CHAT_PATH) != 0 &&
        (context->json_request || (context->upload_data && context->upload_data_size > 0))) {
        return handle_post_jsonrpc(gateway, connection, context);
    }

    /* 阶段 4: 路由到其他处理函数（动态端点优先） */
    int dynamic_ret = handle_dynamic_endpoint_route(gateway, connection, context, method, url);
    if (dynamic_ret != MHD_NO) {
        return dynamic_ret;
    }

    /* 阶段 5: 静态路由表 */
    int (*route_handler)(http_gateway_t *, struct MHD_Connection *, http_request_context_t *) =
        find_http_route(method, url);

    if (route_handler) {
        return route_handler(gateway, connection, context);
    }

    /* 阶段 6: 404 Not Found */
    return handle_not_found(gateway, connection, context);
}
