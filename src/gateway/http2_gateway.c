/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file http2_gateway.c
 * @brief HTTP/2 网关实现 — 基于 nghttp2 的 HTTP/2 服务器
 *
 * 实现完整的 HTTP/2 协议服务器，复用 HTTP/1.1 网关的协议处理逻辑
 * （gateway_protocol_handle_request / gateway_rpc_handle_request）。
 *
 * 核心流程：
 *   1. TCP 监听 socket（非阻塞）
 *   2. poll() 事件循环（专用 pthread）
 *   3. 每连接一个 nghttp2 服务端会话
 *   4. nghttp2 回调收集 HTTP/2 请求头和 DATA 帧
 *   5. END_STREAM 触发请求处理 → 生成 JSON 响应
 *   6. nghttp2_submit_response 提交响应（data_provider 回调送数据）
 *
 * IRON-2 铁律：禁止桩函数和简化功能，必须实现真正可用的 HTTP/2 服务器。
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"

#include "../../../commons/utils/error/include/error.h"
#include "../utils/gateway_protocol_handler.h"
#include "../utils/gateway_rate_limiter.h"
#include "../utils/gateway_rpc_handler.h"
#include "../utils/gateway_utils.h"
#include "../utils/jsonrpc.h"
#include "airy_memory.h"
#include "logging.h"

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 跨平台原子操作支持 */
#include "atomic_compat.h"

/* ========== 平台特定头文件 ========== */
#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifdef AIRY_HAS_HTTP2

/* ========== 常量定义 ========== */

#define HTTP2_RECV_BUF_SIZE (64 * 1024)       /**< 接收缓冲区大小 */
#define HTTP2_SEND_BUF_SIZE (64 * 1024)       /**< 发送缓冲区大小 */
#define HTTP2_MAX_HEADER_SIZE (64 * 1024)     /**< 单个请求最大头大小 */
#define HTTP2_DEFAULT_MAX_STREAMS 128         /**< 默认最大并发流 */
#define HTTP2_DEFAULT_TIMEOUT_SEC 60          /**< 默认连接空闲超时 */
#define HTTP2_POLL_TIMEOUT_MS 1000            /**< poll 超时（用于检查运行标志） */
#define HTTP2_LISTEN_BACKLOG 128              /**< listen backlog */
#define HTTP2_INITIAL_WINDOW_SIZE 65535       /**< 初始流窗口大小 */

/* ========== 内部类型定义 ========== */

/**
 * @brief 内部处理器适配器
 *
 * 将 gateway_internal_handler_t（内部签名）适配为
 * gateway_protocol_handle_request 所需的 custom_handler 签名。
 */
typedef struct {
    gateway_internal_handler_t internal_handler;
    void *internal_data;
} http2_handler_adapter_t;

/* ========== 前向声明 ========== */

static int http2_gateway_start_impl(void *impl);
static void http2_gateway_stop_impl(void *impl);
static void http2_gateway_destroy_impl(void *impl);
static const char *http2_gateway_get_name_impl(void *impl);
static airy_err_t http2_gateway_get_stats_impl(void *impl, char **out_json);
static bool http2_gateway_is_running_impl(void *impl);
static airy_err_t http2_gateway_set_handler_impl(void *impl,
                                                  gateway_internal_handler_t handler,
                                                  void *user_data);
static void http2_free_response_headers(nghttp2_nv *nva, size_t count);

/* ========== 流上下文管理 ========== */

/**
 * @brief 创建 HTTP/2 流上下文
 */
static http2_stream_context_t *http2_stream_create(int32_t stream_id)
{
    http2_stream_context_t *ctx = AIRY_CALLOC(1, sizeof(http2_stream_context_t));
    if (!ctx) {
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "stream context allocation failed");
        return NULL;
    }
    ctx->stream_id = stream_id;
    ctx->response_status = 200;
    LOG_DEBUG("stream created: stream_id=%d", stream_id);
    return ctx;
}

/**
 * @brief 销毁 HTTP/2 流上下文
 */
static void http2_stream_destroy(http2_stream_context_t *ctx)
{
    if (!ctx)
        return;

    LOG_DEBUG("stream destroyed: stream_id=%d, req_body=%zuB, resp_body=%zuB, status=%d",
              ctx->stream_id, ctx->request_body_len, ctx->response_body_len, ctx->response_status);

    AIRY_FREE(ctx->method);
    AIRY_FREE(ctx->path);
    AIRY_FREE(ctx->content_type);
    AIRY_FREE(ctx->origin);
    AIRY_FREE(ctx->request_body);
    AIRY_FREE(ctx->response_body);
    AIRY_FREE(ctx);
}

/**
 * @brief 追加数据到请求体缓冲区
 * @return 0 成功，负数错误码
 */
static int http2_stream_append_body(http2_stream_context_t *ctx, const uint8_t *data, size_t len)
{
    if (!ctx || (!data && len > 0))
        return AIRY_ERR_NULL_POINTER;

    if (len == 0)
        return 0;

    /* 检查是否需要扩容 */
    if (ctx->request_body_len + len > ctx->request_body_cap) {
        size_t new_cap = ctx->request_body_cap == 0 ? 4096 : ctx->request_body_cap;
        while (new_cap < ctx->request_body_len + len) {
            new_cap *= 2;
            if (new_cap == 0) {
                airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                                 "body buffer overflow");
                return AIRY_ERR_OUT_OF_MEMORY;
            }
        }
        uint8_t *new_buf = AIRY_REALLOC(ctx->request_body, new_cap);
        if (!new_buf) {
            airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                             "body buffer realloc failed");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        ctx->request_body = new_buf;
        ctx->request_body_cap = new_cap;
    }

    AIRY_MEMCPY(ctx->request_body + ctx->request_body_len, data, len);
    ctx->request_body_len += len;
    return 0;
}

/**
 * @brief 安全复制 header 值字符串
 */
static int http2_stream_set_header_str(char **dst, const uint8_t *value, size_t vlen)
{
    if (!dst || !value)
        return AIRY_ERR_NULL_POINTER;

    char *copy = AIRY_MALLOC(vlen + 1);
    if (!copy) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    AIRY_MEMCPY(copy, value, vlen);
    copy[vlen] = '\0';

    AIRY_FREE(*dst);
    *dst = copy;
    return 0;
}

/* ========== 内部处理器适配器 ========== */

/**
 * @brief 适配器：将 gateway_internal_handler_t 转换为 protocol_handle_request 的回调签名
 */
static int http2_internal_handler_adapter(const char *request_json, char **response_json,
                                          void *user_data)
{
    http2_handler_adapter_t *adapter = (http2_handler_adapter_t *)user_data;
    if (!adapter || !adapter->internal_handler) {
        *response_json = NULL;
        airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__,
                         "adapter or internal handler is NULL");
        return AIRY_ERR_NULL_POINTER;
    }

    char *resp = adapter->internal_handler((void *)request_json, adapter->internal_data);
    if (resp) {
        *response_json = resp;
        return 0;
    }
    *response_json = NULL;
    airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__,
                     "internal handler returned NULL");
    return AIRY_ERR_NULL_POINTER;
}

/* ========== 响应生成与提交 ========== */

/**
 * @brief 数据源读取回调 — 向 nghttp2 提供 HTTP/2 DATA 帧的响应体
 *
 * 从流上下文的 response_body 中读取数据，拷贝到 nghttp2 提供的 buf 中。
 * 当所有数据读取完毕时设置 NGHTTP2_DATA_FLAG_EOF。
 */
static ssize_t http2_data_source_read_callback(nghttp2_session *session,
                                               int32_t stream_id,
                                               uint8_t *buf, size_t length,
                                               uint32_t *data_flags,
                                               nghttp2_data_source *source,
                                               void *user_data)
{
    (void)session;
    (void)stream_id;
    (void)source;

    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (!ctx) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t remaining = ctx->response_body_len - ctx->response_sent;
    if (remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t to_copy = remaining < length ? remaining : length;
    AIRY_MEMCPY(buf, ctx->response_body + ctx->response_sent, to_copy);
    ctx->response_sent += to_copy;

    if (ctx->response_sent >= ctx->response_body_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (ssize_t)to_copy;
}

/**
 * @brief 构建 HTTP/2 响应头（nghttp2_nv 数组）
 *
 * 包含 :status 伪头、content-type、安全头和 CORS 头。
 *
 * @param ctx 流上下文（用于读取 origin）
 * @param gw 网关实例（用于 CORS 配置）
 * @param nva 输出头数组（调用者分配）
 * @param max_nva 数组最大容量
 * @return 实际填充的头数量
 */
static size_t http2_build_response_headers(http2_stream_context_t *ctx,
                                           http2_gateway_t *gw, nghttp2_nv *nva,
                                           size_t max_nva)
{
    size_t count = 0;
    char status_str[8];
    http_gateway_t *base = &gw->base;

    /* :status 伪头 */
    snprintf(status_str, sizeof(status_str), "%d", ctx->response_status);
    if (count < max_nva) {
        nva[count].name = (uint8_t *)":status";
        nva[count].namelen = 7;
        nva[count].value = (uint8_t *)AIRY_STRDUP(status_str);
        if (!nva[count].value) {
            /* P0: OOM — 清理已分配项并返回错误，调用方不得提交 value=NULL 的 nv */
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = strlen(status_str);
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }

    /* content-type */
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"content-type";
        nva[count].namelen = 12;
        nva[count].value = (uint8_t *)AIRY_STRDUP("application/json");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 16;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }

    /* server */
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"server";
        nva[count].namelen = 6;
        nva[count].value = (uint8_t *)AIRY_STRDUP("AgentRT-gateway/2.0");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 19;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }

    /* 安全头 */
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"x-content-type-options";
        nva[count].namelen = 22;
        nva[count].value = (uint8_t *)AIRY_STRDUP("nosniff");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 7;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"x-frame-options";
        nva[count].namelen = 15;
        nva[count].value = (uint8_t *)AIRY_STRDUP("DENY");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 4;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"strict-transport-security";
        nva[count].namelen = 25;
        nva[count].value = (uint8_t *)AIRY_STRDUP("max-age=31536000; includeSubDomains");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 35;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"cache-control";
        nva[count].namelen = 13;
        nva[count].value = (uint8_t *)AIRY_STRDUP("no-store, no-cache, must-revalidate");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 34;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }

    /* CORS 头 */
    if (ctx->origin && base) {
        bool origin_allowed = false;
        if (base->cors.allow_all_origins) {
            origin_allowed = true;
        } else {
            for (size_t i = 0; i < base->cors.allowed_origins_count; i++) {
                if (base->cors.allowed_origins[i] &&
                    strcmp(ctx->origin, base->cors.allowed_origins[i]) == 0) {
                    origin_allowed = true;
                    break;
                }
            }
        }

        if (origin_allowed && count < max_nva) {
            nva[count].name = (uint8_t *)"access-control-allow-origin";
            nva[count].namelen = 27;
            nva[count].value = (uint8_t *)AIRY_STRDUP(ctx->origin);
            if (!nva[count].value) {
                http2_free_response_headers(nva, count);
                return (size_t)-1;
            }
            nva[count].valuelen = strlen(ctx->origin);
            nva[count].flags = NGHTTP2_NV_FLAG_NONE;
            count++;
        }
    }

    return count;
}

/**
 * @brief 释放响应头数组中动态分配的 value 字符串
 */
static void http2_free_response_headers(nghttp2_nv *nva, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        AIRY_FREE(nva[i].value);
    }
}

/**
 * @brief 提交 HTTP/2 响应
 *
 * 构建响应头，设置 data_provider，调用 nghttp2_submit_response。
 * 响应体通过 data_source_read_callback 异步发送。
 */
static int http2_submit_response_impl(nghttp2_session *session, http2_stream_context_t *ctx,
                                      http2_gateway_t *gw)
{
    if (!ctx || !session) {
        return NGHTTP2_ERR_INVALID_ARGUMENT;
    }

    /* 如果没有响应体，生成空 JSON */
    if (!ctx->response_body) {
        ctx->response_body = AIRY_STRDUP("{}");
        ctx->response_body_len = 2;
    }

    /* 构建响应头 */
    nghttp2_nv nva[16];
    size_t nvlen = http2_build_response_headers(ctx, gw, nva, 16);
    if (nvlen == (size_t)-1) {
        /* P0: 头构建 OOM — 已清理部分分配，不得提交 value=NULL 的非法 nv；
         * 终止该流，避免 nghttp2_submit_response 崩溃 */
        LOG_ERROR("failed to build response headers (stream_id=%d)", ctx->stream_id);
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, ctx->stream_id,
                                  NGHTTP2_INTERNAL_ERROR);
        ctx->response_sent_flag = true;
        return NGHTTP2_ERR_NOMEM;
    }

    /* 设置 data provider */
    nghttp2_data_provider data_prd;
    data_prd.source.ptr = ctx;
    data_prd.read_callback = http2_data_source_read_callback;

    int ret = nghttp2_submit_response(session, ctx->stream_id, nva, nvlen, &data_prd);

    http2_free_response_headers(nva, nvlen);

    if (ret != 0) {
        LOG_ERROR("nghttp2_submit_response failed: %s (stream_id=%d)", nghttp2_strerror(ret),
                  ctx->stream_id);
    } else {
        ctx->response_sent_flag = true;
    }

    return ret;
}

/* ========== 请求处理 ========== */

/**
 * @brief 处理 JSON-RPC POST 请求
 *
 * 复用 gateway_protocol_handle_request 进行多协议处理，
 * 回退到 gateway_rpc_handle_request 处理已解析的 JSON。
 */
static char *http2_handle_jsonrpc(http2_gateway_t *gw, http2_stream_context_t *ctx)
{
    http_gateway_t *base = &gw->base;

    /* 优先使用多协议处理器处理原始请求体 */
    if (base->protocol_handler && ctx->request_body && ctx->request_body_len > 0) {
        http2_handler_adapter_t adapter = {.internal_handler = base->handler,
                                           .internal_data = base->handler_data};

        rpc_result_t result = gateway_protocol_handle_request(
            base->protocol_handler, (const char *)ctx->request_body, ctx->request_body_len,
            AIRY_PROTOCOL_COUNT,
            base->handler ? http2_internal_handler_adapter : NULL,
            base->handler ? &adapter : NULL);

        if (result.error_code != 0 || !result.response_json) {
            char *error_resp =
                result.response_json ? result.response_json
                                     : jsonrpc_create_error_response(NULL, -32603,
                                                                     "Internal error", NULL);
            if (result.response_json)
                result.response_json = NULL;
            gateway_rpc_free(&result);
            return error_resp;
        }

        char *success_resp = result.response_json;
        result.response_json = NULL;
        gateway_rpc_free(&result);
        return success_resp;
    }

    /* 回退：无协议处理器或无请求体 */
    return jsonrpc_create_error_response(NULL, -32600, "Invalid request", NULL);
}

/**
 * @brief 处理健康检查请求 GET /health
 */
static char *http2_handle_health(void)
{
    return AIRY_STRDUP("{\"status\":\"healthy\",\"service\":\"gateway\",\"protocol\":\"h2\"}");
}

/**
 * @brief 处理 OPTIONS 预检请求
 */
static char *http2_handle_preflight(void)
{
    return AIRY_STRDUP("");
}

/**
 * @brief 处理完整请求并提交响应
 *
 * 在 END_STREAM 收到后调用，根据 method + path 路由到对应的处理函数，
 * 生成 JSON 响应，然后通过 nghttp2_submit_response 提交。
 */
static void http2_process_request(nghttp2_session *session, int32_t stream_id, void *user_data)
{
    http2_gateway_t *gw = (http2_gateway_t *)user_data;
    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);

    if (!ctx || ctx->response_sent_flag) {
        LOG_WARN("process_request skipped: ctx=%p, already_sent=%d", (void *)ctx,
                 ctx ? ctx->response_sent_flag : 0);
        return;
    }

    char *response_json = NULL;

    LOG_INFO("processing request: stream_id=%d, method=%s, path=%s, body_len=%zu",
             ctx->stream_id, ctx->method ? ctx->method : "(null)",
             ctx->path ? ctx->path : "(null)", ctx->request_body_len);

    /* 路由分发 */
    if (ctx->method && strcmp(ctx->method, "POST") == 0) {
        /* POST / → JSON-RPC */
        if (ctx->request_body_len > gw->base.max_request_size) {
            LOG_WARN("request too large: %zu > %zu (stream_id=%d)",
                     ctx->request_body_len, gw->base.max_request_size, ctx->stream_id);
            ctx->response_status = 413;
            response_json = jsonrpc_create_error_response(NULL, -413, "Request too large", NULL);
        } else {
            response_json = http2_handle_jsonrpc(gw, ctx);
            if (!response_json) {
                LOG_ERROR("jsonrpc handler returned NULL (stream_id=%d)", ctx->stream_id);
                ctx->response_status = 500;
                response_json = jsonrpc_create_error_response(NULL, -32603,
                                                              "Internal error", NULL);
            }
        }
    } else if (ctx->method && strcmp(ctx->method, "GET") == 0) {
        if (ctx->path && strcmp(ctx->path, "/health") == 0) {
            LOG_DEBUG("health check request (stream_id=%d)", ctx->stream_id);
            response_json = http2_handle_health();
        } else {
            LOG_WARN("GET path not found: %s (stream_id=%d)",
                     ctx->path ? ctx->path : "(null)", ctx->stream_id);
            ctx->response_status = 404;
            response_json = jsonrpc_create_error_response(NULL, -32601, "Not Found", NULL);
        }
    } else if (ctx->method && strcmp(ctx->method, "OPTIONS") == 0) {
        LOG_DEBUG("OPTIONS preflight request (stream_id=%d)", ctx->stream_id);
        response_json = http2_handle_preflight();
    } else {
        LOG_WARN("unsupported method: %s (stream_id=%d)",
                 ctx->method ? ctx->method : "(null)", ctx->stream_id);
        ctx->response_status = 404;
        response_json = jsonrpc_create_error_response(NULL, -32601, "Not Found", NULL);
    }

    /* 设置响应体 */
    if (response_json) {
        ctx->response_body = response_json;
        ctx->response_body_len = strlen(response_json);
    } else {
        ctx->response_body = AIRY_STRDUP("{}");
        ctx->response_body_len = 2;
    }

    ctx->response_sent = 0;

    /* 更新统计 */
    atomic_fetch_add(&gw->base.requests_total, 1);
    atomic_fetch_add(&gw->base.bytes_received, ctx->request_body_len);
    atomic_fetch_add(&gw->base.bytes_sent, ctx->response_body_len);

    /* 提交响应 */
    int submit_ret = http2_submit_response_impl(session, ctx, gw);
    LOG_INFO("response submitted: stream_id=%d, status=%d, resp_len=%zu, ret=%d",
             ctx->stream_id, ctx->response_status, ctx->response_body_len, submit_ret);
}

/* ========== nghttp2 回调实现 ========== */

/**
 * @brief on_begin_headers 回调 — 分配流上下文
 */
static int http2_on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame,
                                  void *user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    http2_stream_context_t *ctx = http2_stream_create(frame->hd.stream_id);
    if (!ctx) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    int ret = nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, ctx);
    if (ret != 0) {
        http2_stream_destroy(ctx);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    (void)user_data;
    return 0;
}

/**
 * @brief on_header 回调 — 收集请求头
 */
static int http2_on_header(nghttp2_session *session, const nghttp2_frame *frame,
                           const uint8_t *name, size_t namelen, const uint8_t *value,
                           size_t valuelen, uint8_t flags, void *user_data)
{
    (void)flags;
    (void)user_data;

    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session,
                                                                       frame->hd.stream_id);
    if (!ctx)
        return 0;

    /* 处理伪头 */
    if (namelen > 0 && name[0] == ':') {
        if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
            return http2_stream_set_header_str(&ctx->method, value, valuelen);
        }
        if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
            return http2_stream_set_header_str(&ctx->path, value, valuelen);
        }
        return 0;
    }

    /* 处理常规头 */
    if (namelen == 12 && strncasecmp((const char *)name, "content-type", 12) == 0) {
        return http2_stream_set_header_str(&ctx->content_type, value, valuelen);
    }
    if (namelen == 6 && strncasecmp((const char *)name, "origin", 6) == 0) {
        return http2_stream_set_header_str(&ctx->origin, value, valuelen);
    }

    return 0;
}

/**
 * @brief on_data_chunk_recv 回调 — 接收 DATA 帧数据块
 */
static int http2_on_data_chunk_recv(nghttp2_session *session, uint8_t flags,
                                    int32_t stream_id, const uint8_t *data, size_t len,
                                    void *user_data)
{
    (void)flags;

    http2_gateway_t *gw = (http2_gateway_t *)user_data;
    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (!ctx)
        return 0;

    /* P0: 请求体无上限时，超大 body 会在完整接收后才被 max_request_size
     * 校验拦截，导致内存耗尽 DoS。此处按 gw->base.max_request_size 在累加
     * 内存前限流，超限立即 RST_STREAM 终止该流。 */
    if (gw && ctx->request_body_len + len > gw->base.max_request_size) {
        LOG_WARN("request body exceeds limit: %zu + %zu > %zu (stream_id=%d)",
                 ctx->request_body_len, len, gw->base.max_request_size, stream_id);
        ctx->response_status = 413;
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id,
                                  NGHTTP2_CANCEL);
        return 0;
    }

    int ret = http2_stream_append_body(ctx, data, len);
    if (ret != 0) {
        LOG_ERROR("Failed to append body data: %d (stream_id=%d)", ret, stream_id);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    return 0;
}

/**
 * @brief on_frame_recv 回调 — 帧接收完成
 *
 * 检测 HEADERS 或 DATA 帧的 END_STREAM 标志，
 * 触发请求处理。
 */
static int http2_on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame,
                               void *user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) {
        return 0;
    }

    if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        return 0;
    }

    /* END_STREAM 收到，请求完整，处理请求 */
    http2_process_request(session, frame->hd.stream_id, user_data);
    return 0;
}

/**
 * @brief on_stream_close 回调 — 流关闭，清理资源
 */
static int http2_on_stream_close(nghttp2_session *session, int32_t stream_id,
                                 uint32_t error_code, void *user_data)
{
    (void)user_data;

    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (ctx) {
        if (error_code != NGHTTP2_NO_ERROR) {
            LOG_WARN("stream closed with error: stream_id=%d, error_code=%u (%s)",
                     stream_id, error_code, nghttp2_http2_strerror(error_code));
        } else {
            LOG_DEBUG("stream closed normally: stream_id=%d", stream_id);
        }
        http2_stream_destroy(ctx);
        nghttp2_session_set_stream_user_data(session, stream_id, NULL);
    }

    return 0;
}

/* ========== 会话管理 ========== */

/**
 * @brief 创建 nghttp2 回调表
 */
static int http2_create_callbacks(nghttp2_session_callbacks **callbacks)
{
    int ret = nghttp2_session_callbacks_new(callbacks);
    if (ret != 0) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "nghttp2_session_callbacks_new failed");
        return ret;
    }

    nghttp2_session_callbacks_set_on_begin_headers_callback(*callbacks, http2_on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(*callbacks, http2_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(*callbacks,
                                                              http2_on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_frame_recv_callback(*callbacks, http2_on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(*callbacks, http2_on_stream_close);

    return 0;
}

/**
 * @brief 创建新的 HTTP/2 会话
 */
static http2_gateway_session_t *http2_session_create(http2_gateway_t *gw, int fd)
{
    http2_gateway_session_t *sess = AIRY_CALLOC(1, sizeof(http2_gateway_session_t));
    if (!sess) {
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "session allocation failed");
        return NULL;
    }

    sess->fd = fd;
    sess->gateway = gw;
    sess->connect_time_ns = gateway_time_ns();
    sess->last_activity_ns = sess->connect_time_ns;
    sess->closing = false;

    /* 创建 nghttp2 回调表 */
    nghttp2_session_callbacks *callbacks = NULL;
    if (http2_create_callbacks(&callbacks) != 0) {
        AIRY_FREE(sess);
        return NULL;
    }

    /* 创建服务端会话 */
    int ret = nghttp2_session_server_new(&sess->session, callbacks, gw);
    nghttp2_session_callbacks_del(callbacks);

    if (ret != 0) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "nghttp2_session_server_new failed: %s", nghttp2_strerror(ret));
        AIRY_FREE(sess);
        return NULL;
    }

    /* 提交初始 SETTINGS 帧 */
    nghttp2_settings_entry settings_entries[3];
    size_t num_entries = 0;

    settings_entries[num_entries].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    settings_entries[num_entries].value = gw->max_concurrent_streams;
    num_entries++;

    settings_entries[num_entries].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
    settings_entries[num_entries].value = HTTP2_INITIAL_WINDOW_SIZE;
    num_entries++;

    settings_entries[num_entries].settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
    settings_entries[num_entries].value = HTTP2_MAX_HEADER_SIZE;
    num_entries++;

    ret = nghttp2_submit_settings(sess->session, NGHTTP2_FLAG_NONE, settings_entries,
                                  num_entries);
    if (ret != 0) {
        LOG_ERROR("nghttp2_submit_settings failed: %s (fd=%d)", nghttp2_strerror(ret), fd);
        nghttp2_session_del(sess->session);
        AIRY_FREE(sess);
        return NULL;
    }

    LOG_INFO("HTTP/2 session created: fd=%d, max_streams=%u, window=%d",
             fd, gw->max_concurrent_streams, HTTP2_INITIAL_WINDOW_SIZE);
    return sess;
}

/**
 * @brief 销毁 HTTP/2 会话
 */
static void http2_session_destroy(http2_gateway_session_t *sess)
{
    if (!sess)
        return;

    LOG_INFO("HTTP/2 session destroying: fd=%d", sess->fd);

    if (sess->session) {
        nghttp2_session_del(sess->session);
        sess->session = NULL;
    }

    if (sess->fd >= 0) {
        close(sess->fd);
        sess->fd = -1;
    }

    /* P0 修复: 释放部分写入缓冲区 */
    if (sess->pending_send_buf) {
        AIRY_FREE(sess->pending_send_buf);
        sess->pending_send_buf = NULL;
        sess->pending_send_len = 0;
        sess->pending_send_offset = 0;
    }

    AIRY_FREE(sess);
}

/* ========== 会话 I/O ========== */

/**
 * @brief 从 socket 读取数据并送入 nghttp2 处理
 * @return 0 正常，1 连接关闭，负数错误
 */
static int http2_session_recv_data(http2_gateway_session_t *sess)
{
    uint8_t buf[HTTP2_RECV_BUF_SIZE];

    ssize_t nread = read(sess->fd, buf, sizeof(buf));
    if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        LOG_ERROR("read failed on fd %d: %s", sess->fd, strerror(errno));
        return -1;
    }

    if (nread == 0) {
        /* 对端关闭连接 */
        LOG_INFO("peer closed connection: fd=%d", sess->fd);
        return 1;
    }

    sess->last_activity_ns = gateway_time_ns();
    atomic_fetch_add(&sess->gateway->base.bytes_received, (uint64_t)nread);
    LOG_DEBUG("recv %zd bytes on fd=%d", nread, sess->fd);

    /* 送入 nghttp2 处理 */
    ssize_t processed = nghttp2_session_mem_recv(sess->session, buf, (size_t)nread);
    if (processed < 0) {
        LOG_ERROR("nghttp2_session_mem_recv failed: %s (fd=%d, processed=%zd/%zd)",
                  nghttp2_strerror((int)processed), sess->fd, processed, nread);
        return -1;
    }

    /* nghttp2 可能未消费全部数据（如连接前言中多余字节），记录但不视为错误 */
    if ((size_t)processed < (size_t)nread) {
        LOG_DEBUG("nghttp2 partial recv: processed=%zd/%zd bytes (fd=%d)",
                  processed, nread, sess->fd);
    }

    return 0;
}

/**
 * @brief 将 nghttp2 待发送数据写入 socket
 *
 * P0 修复: 原实现在 write() 部分写入时直接调用 nghttp2_session_mem_send()
 * 获取下一块数据，导致未写完的数据丢失。修复方案：
 *   1. 先尝试 flush pending_send_buf 中的残留数据
 *   2. 只有当 pending_send_buf 为空时才调用 nghttp2_session_mem_send
 *   3. 如果 write() 部分写入，将剩余数据缓存到 pending_send_buf
 *   4. 下次 POLLOUT 时从 pending_send_buf 继续发送
 *
 * @return 0 正常，负数错误
 */
static int http2_session_send_data(http2_gateway_session_t *sess)
{
    /* Step 1: 先 flush pending buffer 中的残留数据 */
    if (sess->pending_send_buf && sess->pending_send_offset < sess->pending_send_len) {
        size_t remaining = sess->pending_send_len - sess->pending_send_offset;
        ssize_t written = write(sess->fd, sess->pending_send_buf + sess->pending_send_offset,
                                remaining);

        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                LOG_DEBUG("pending flush deferred: fd=%d, remaining=%zu", sess->fd, remaining);
                return 0;
            }
            LOG_ERROR("pending write failed on fd %d: %s", sess->fd, strerror(errno));
            return -1;
        }

        sess->pending_send_offset += (size_t)written;
        sess->last_activity_ns = gateway_time_ns();
        atomic_fetch_add(&sess->gateway->base.bytes_sent, (uint64_t)written);
        LOG_DEBUG("pending flush: wrote %zd/%zu bytes (fd=%d)", written, remaining, sess->fd);

        if (sess->pending_send_offset < sess->pending_send_len) {
            /* Socket buffer 仍满，等下次 POLLOUT */
            return 0;
        }

        /* Pending buffer 全部发送完毕，释放 */
        AIRY_FREE(sess->pending_send_buf);
        sess->pending_send_buf = NULL;
        sess->pending_send_len = 0;
        sess->pending_send_offset = 0;
    }

    /* Step 2: 从 nghttp2 获取新数据并发送 */
    const uint8_t *data_ptr = NULL;
    ssize_t send_len = nghttp2_session_mem_send(sess->session, &data_ptr);

    while (send_len > 0 && data_ptr) {
        size_t total = (size_t)send_len;
        size_t offset = 0;

        /* 尝试写入全部数据 */
        while (offset < total) {
            ssize_t written = write(sess->fd, data_ptr + offset, total - offset);

            if (written < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    /* Socket buffer 满，将剩余数据缓存到 pending_send_buf */
                    size_t remaining = total - offset;
                    sess->pending_send_buf = AIRY_MALLOC(remaining);
                    if (!sess->pending_send_buf) {
                        LOG_ERROR("pending buffer alloc failed: fd=%d, size=%zu",
                                  sess->fd, remaining);
                        return -1;
                    }
                    memcpy(sess->pending_send_buf, data_ptr + offset, remaining);
                    sess->pending_send_len = remaining;
                    sess->pending_send_offset = 0;
                    LOG_WARN("partial write: buffered %zu bytes for fd=%d (total=%zu, sent=%zu)",
                             remaining, sess->fd, total, offset);
                    return 0;
                }
                LOG_ERROR("write failed on fd %d: %s", sess->fd, strerror(errno));
                return -1;
            }

            offset += (size_t)written;
        }

        sess->last_activity_ns = gateway_time_ns();
        atomic_fetch_add(&sess->gateway->base.bytes_sent, (uint64_t)total);
        LOG_DEBUG("send %zu bytes on fd=%d", total, sess->fd);

        /* 当前 chunk 全部发送完毕，获取下一块 */
        send_len = nghttp2_session_mem_send(sess->session, &data_ptr);
    }

    if (send_len < 0) {
        LOG_ERROR("nghttp2_session_mem_send failed: %s (fd=%d)",
                  nghttp2_strerror((int)send_len), sess->fd);
        return -1;
    }

    return 0;
}

/* ========== 会话数组管理 ========== */

/**
 * @brief 添加会话到网关的会话数组
 */
static int http2_gateway_add_session(http2_gateway_t *gw, http2_gateway_session_t *sess)
{
    if (gw->session_count >= gw->session_capacity) {
        size_t new_cap = gw->session_capacity == 0 ? 16 : gw->session_capacity * 2;
        http2_gateway_session_t **new_arr =
            AIRY_REALLOC(gw->sessions, new_cap * sizeof(http2_gateway_session_t *));
        if (!new_arr) {
            airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                             "session array realloc failed");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        gw->sessions = new_arr;
        gw->session_capacity = new_cap;
    }

    gw->sessions[gw->session_count++] = sess;
    return 0;
}

/**
 * @brief 从会话数组中移除并销毁指定索引的会话
 */
static void http2_gateway_remove_session(http2_gateway_t *gw, size_t index)
{
    if (index >= gw->session_count)
        return;

    http2_session_destroy(gw->sessions[index]);

    /* 将最后一个元素移到被删除的位置 */
    gw->session_count--;
    if (index < gw->session_count) {
        gw->sessions[index] = gw->sessions[gw->session_count];
    }
    gw->sessions[gw->session_count] = NULL;
}

/* ========== 事件循环 ========== */

/**
 * @brief 接受新连接
 */
static void http2_event_loop_accept(http2_gateway_t *gw)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int fd = accept(gw->listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            LOG_ERROR("accept failed: %s", strerror(errno));
        }
        return;
    }

    /* 连接数限制 */
    if (gw->session_count >= gw->max_concurrent_streams) {
        LOG_WARN("connection limit reached (%zu/%u), rejecting new connection",
                 gw->session_count, gw->max_concurrent_streams);
        close(fd);
        return;
    }

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* 设置 TCP_NODELAY */
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* 创建会话 */
    http2_gateway_session_t *sess = http2_session_create(gw, fd);
    if (!sess) {
        close(fd);
        return;
    }

    /* 立即发送 SETTINGS 帧 */
    if (http2_session_send_data(sess) != 0) {
        http2_session_destroy(sess);
        return;
    }

    if (http2_gateway_add_session(gw, sess) != 0) {
        http2_session_destroy(sess);
        return;
    }

    char ip_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf));
    LOG_INFO("connection accepted: %s:%d → fd=%d (sessions=%zu/%u)",
             ip_buf, ntohs(addr.sin_port), fd, gw->session_count, gw->max_concurrent_streams);
}

/**
 * @brief 检查会话超时并清理
 */
static void http2_event_loop_cleanup(http2_gateway_t *gw)
{
    uint64_t now = gateway_time_ns();
    uint64_t timeout_ns = (uint64_t)gw->connection_timeout * 1000000000ULL;

    for (size_t i = 0; i < gw->session_count; ) {
        http2_gateway_session_t *sess = gw->sessions[i];

        /* 检查会话是否应该关闭 */
        bool should_close = sess->closing;

        /* 检查超时 */
        if (!should_close && timeout_ns > 0) {
            if ((now - sess->last_activity_ns) > timeout_ns) {
                LOG_INFO("session timeout: fd=%d, idle=%llums",
                         sess->fd,
                         (unsigned long long)((now - sess->last_activity_ns) / 1000000ULL));
                should_close = true;
            }
        }

        /* 检查 nghttp2 是否还想读/写 */
        if (!should_close) {
            if (!nghttp2_session_want_read(sess->session) &&
                !nghttp2_session_want_write(sess->session)) {
                LOG_DEBUG("nghttp2 session done: fd=%d (no more read/write)", sess->fd);
                should_close = true;
            }
        }

        if (should_close) {
            /* 检查是否有未发送完毕的 pending buffer */
            if (sess->pending_send_buf && sess->pending_send_offset < sess->pending_send_len) {
                LOG_WARN("closing session with %zu bytes unsent: fd=%d",
                         sess->pending_send_len - sess->pending_send_offset, sess->fd);
            }
            /* 发送 GOAWAY 帧 */
            if (sess->session) {
                nghttp2_submit_goaway(sess->session, NGHTTP2_FLAG_NONE,
                                      nghttp2_session_get_last_proc_stream_id(sess->session),
                                      NGHTTP2_NO_ERROR, NULL, 0);
                http2_session_send_data(sess);
            }
            http2_gateway_remove_session(gw, i);
        } else {
            i++;
        }
    }
}

/**
 * @brief 事件循环主函数（在专用线程中运行）
 */
static void *http2_event_loop(void *arg)
{
    http2_gateway_t *gw = (http2_gateway_t *)arg;

    LOG_INFO("HTTP/2 event loop started (port=%u)", gw->base.port);

    while (atomic_load(&gw->running)) {
        /* 构建 pollfd 数组：listen_fd + 所有会话 fd */
        size_t max_fds = gw->session_count + 1;
        struct pollfd *fds = AIRY_CALLOC(max_fds, sizeof(struct pollfd));
        if (!fds) {
            /* 内存不足，等待后重试 */
            gateway_sleep(1);
            continue;
        }

        nfds_t nfds = 0;

        /* 监听 socket */
        fds[nfds].fd = gw->listen_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        /* 会话 socket */
        for (size_t i = 0; i < gw->session_count; i++) {
            http2_gateway_session_t *sess = gw->sessions[i];
            short events = 0;

            if (nghttp2_session_want_read(sess->session)) {
                events |= POLLIN;
            }
            if (nghttp2_session_want_write(sess->session)) {
                events |= POLLOUT;
            }

            /* P0 修复: 如果有 pending_send_buf 未发送完毕，必须监听 POLLOUT */
            if (sess->pending_send_buf && sess->pending_send_offset < sess->pending_send_len) {
                events |= POLLOUT;
            }

            /* 如果既不想读也不想写，但会话还活着，仍然监听读（用于检测对端关闭） */
            if (events == 0) {
                events = POLLIN;
            }

            fds[nfds].fd = sess->fd;
            fds[nfds].events = events;
            nfds++;
        }

        int ret = poll(fds, nfds, HTTP2_POLL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR) {
                AIRY_FREE(fds);
                continue;
            }
            LOG_ERROR("poll failed: %s", strerror(errno));
            AIRY_FREE(fds);
            break;
        }

        /* 处理新连接 */
        if (fds[0].revents & POLLIN) {
            http2_event_loop_accept(gw);
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_ERROR("Listen socket error");
            AIRY_FREE(fds);
            break;
        }

        /* 处理会话 I/O */
        for (size_t i = 0; i < gw->session_count && i + 1 < nfds; ) {
            http2_gateway_session_t *sess = gw->sessions[i];
            short revents = fds[i + 1].revents;

            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                /* 连接错误或挂起，关闭会话 */
                LOG_WARN("session socket error: fd=%d, revents=0x%x (%s%s%s)",
                         sess->fd, revents,
                         (revents & POLLERR) ? "ERR " : "",
                         (revents & POLLHUP) ? "HUP " : "",
                         (revents & POLLNVAL) ? "NVAL" : "");
                http2_gateway_remove_session(gw, i);
                continue;
            }

            bool session_ok = true;

            if (revents & POLLIN) {
                int recv_ret = http2_session_recv_data(sess);
                if (recv_ret < 0) {
                    session_ok = false;
                } else if (recv_ret == 1) {
                    /* 对端关闭 */
                    session_ok = false;
                }
            }

            if (session_ok && (revents & POLLOUT)) {
                if (http2_session_send_data(sess) != 0) {
                    session_ok = false;
                }
            }

            /* mem_recv 后可能有待发送数据（如响应），尝试发送 */
            if (session_ok && (revents & POLLIN)) {
                if (http2_session_send_data(sess) != 0) {
                    session_ok = false;
                }
            }

            if (!session_ok) {
                http2_gateway_remove_session(gw, i);
            } else {
                i++;
            }
        }

        AIRY_FREE(fds);

        /* 清理超时和已完成的会话 */
        http2_event_loop_cleanup(gw);
    }

    /* 清理所有剩余会话 */
    while (gw->session_count > 0) {
        http2_gateway_remove_session(gw, 0);
    }

    LOG_INFO("HTTP/2 event loop stopped");
    return NULL;
}

/* ========== 网关操作表实现 ========== */

/**
 * @brief 设置 socket 为非阻塞模式
 */
static int http2_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief 设置 SO_REUSEADDR
 */
static void http2_set_reuseaddr(int fd)
{
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

static airy_err_t http2_gateway_start_impl(void *impl)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;

    if (atomic_load(&gw->running)) {
        LOG_WARN("HTTP/2 gateway already running");
        return AIRY_EBUSY;
    }

    /* 创建监听 socket */
    gw->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gw->listen_fd < 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__,
                         "socket creation failed: %s", strerror(errno));
        return AIRY_EBUSY;
    }

    http2_set_reuseaddr(gw->listen_fd);

    /* 绑定 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(gw->base.port);

    if (inet_pton(AF_INET, gw->base.host, &addr.sin_addr) != 1) {
        /* 如果 host 不是有效 IP，绑定所有接口 */
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(gw->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__,
                         "bind failed on %s:%u: %s", gw->base.host, gw->base.port,
                         strerror(errno));
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_EBUSY;
    }

    /* 监听 */
    if (listen(gw->listen_fd, HTTP2_LISTEN_BACKLOG) < 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__,
                         "listen failed: %s", strerror(errno));
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_EBUSY;
    }

    /* 设置非阻塞 */
    if (http2_set_nonblocking(gw->listen_fd) < 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__,
                         "failed to set non-blocking on listen socket");
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_EBUSY;
    }

    atomic_store(&gw->running, true);

    /* 启动事件循环线程 */
    pthread_t *thread = AIRY_MALLOC(sizeof(pthread_t));
    if (!thread) {
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "thread allocation failed");
        atomic_store(&gw->running, false);
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    int ret = pthread_create(thread, NULL, http2_event_loop, gw);
    if (ret != 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__,
                         "pthread_create failed: %s", strerror(ret));
        AIRY_FREE(thread);
        atomic_store(&gw->running, false);
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_EBUSY;
    }

    gw->event_thread = thread;

    LOG_INFO("HTTP/2 gateway started on %s:%u (max_streams=%u)",
             gw->base.host, gw->base.port, gw->max_concurrent_streams);

    return AIRY_SUCCESS;
}

static void http2_gateway_stop_impl(void *impl)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;

    if (!atomic_load(&gw->running)) {
        return;
    }

    atomic_store(&gw->running, false);

    /* 关闭监听 socket 以唤醒 poll() */
    if (gw->listen_fd >= 0) {
        close(gw->listen_fd);
        gw->listen_fd = -1;
    }

    /* 等待事件循环线程退出 */
    if (gw->event_thread) {
        pthread_t *thread = (pthread_t *)gw->event_thread;
        pthread_join(*thread, NULL);
        AIRY_FREE(thread);
        gw->event_thread = NULL;
    }

    LOG_INFO("HTTP/2 gateway stopped");
}

static void http2_gateway_destroy_impl(void *impl)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;
    if (!gw)
        return;

    http2_gateway_stop_impl(gw);

    /* 清理会话数组 */
    if (gw->sessions) {
        for (size_t i = 0; i < gw->session_count; i++) {
            http2_session_destroy(gw->sessions[i]);
        }
        AIRY_FREE(gw->sessions);
        gw->sessions = NULL;
    }
    gw->session_count = 0;
    gw->session_capacity = 0;

    /* 清理 base 资源（复用 HTTP/1.1 的清理逻辑） */
    http_gateway_t *base = &gw->base;

    if (base->handler_adapter) {
        AIRY_FREE(base->handler_adapter);
        base->handler_adapter = NULL;
    }
    base->handler = NULL;
    base->handler_data = NULL;

    if (base->host) {
        AIRY_FREE(base->host);
    }

    /* 清理 CORS 配置 */
    if (base->cors.allowed_methods) {
        AIRY_FREE(base->cors.allowed_methods);
    }
    if (base->cors.allowed_headers) {
        AIRY_FREE(base->cors.allowed_headers);
    }
    if (base->cors.allowed_origins) {
        for (size_t i = 0; i < base->cors.allowed_origins_count; i++) {
            AIRY_FREE(base->cors.allowed_origins[i]);
        }
        AIRY_FREE(base->cors.allowed_origins);
    }

    /* 清理速率限制器 */
    if (base->rate_limiter) {
        gateway_rate_limiter_destroy(base->rate_limiter);
    }

    /* 清理协议处理器 */
    if (base->protocol_handler) {
        gateway_protocol_handler_destroy(base->protocol_handler);
        base->protocol_handler = NULL;
    }

    /* 清理动态端点 */
    if (base->dynamic_endpoints) {
        for (size_t i = 0; i < base->dynamic_endpoint_count; i++) {
            AIRY_FREE(base->dynamic_endpoints[i].method);
            AIRY_FREE(base->dynamic_endpoints[i].path);
        }
        AIRY_FREE(base->dynamic_endpoints);
        base->dynamic_endpoints = NULL;
    }
    base->dynamic_endpoint_count = 0;
    base->dynamic_endpoint_capacity = 0;

    AIRY_FREE(gw);
}

static const char *http2_gateway_get_name_impl(void *impl)
{
    (void)impl;
    return "HTTP/2 Gateway";
}

static airy_err_t http2_gateway_get_stats_impl(void *impl, char **out_json)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;
    if (!gw || !out_json)
        return AIRY_EINVAL;

#ifdef AIRY_HAS_CJSON
    cJSON *stats = cJSON_CreateObject();
    if (!stats)
        return AIRY_ENOMEM;

    cJSON_AddNumberToObject(stats, "requests_total",
                            (double)atomic_load(&gw->base.requests_total));
    cJSON_AddNumberToObject(stats, "requests_failed",
                            (double)atomic_load(&gw->base.requests_failed));
    cJSON_AddNumberToObject(stats, "bytes_received",
                            (double)atomic_load(&gw->base.bytes_received));
    cJSON_AddNumberToObject(stats, "bytes_sent",
                            (double)atomic_load(&gw->base.bytes_sent));
    cJSON_AddNumberToObject(stats, "active_sessions", (double)gw->session_count);
    cJSON_AddNumberToObject(stats, "max_concurrent_streams",
                            (double)gw->max_concurrent_streams);
    cJSON_AddStringToObject(stats, "protocol", "h2");

    char *json_str = cJSON_PrintUnformatted(stats);
    cJSON_Delete(stats);

    if (!json_str)
        return AIRY_ENOMEM;
    *out_json = json_str;
#else
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"requests_total\":%llu,\"requests_failed\":%llu,\"bytes_received\":%llu,"
             "\"bytes_sent\":%llu,\"active_sessions\":%zu,\"protocol\":\"h2\"}",
             (unsigned long long)atomic_load(&gw->base.requests_total),
             (unsigned long long)atomic_load(&gw->base.requests_failed),
             (unsigned long long)atomic_load(&gw->base.bytes_received),
             (unsigned long long)atomic_load(&gw->base.bytes_sent),
             gw->session_count);
    *out_json = AIRY_STRDUP(buf);
#endif

    return AIRY_SUCCESS;
}

static bool http2_gateway_is_running_impl(void *impl)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;
    if (!gw)
        return false;
    return atomic_load(&gw->running);
}

static airy_err_t http2_gateway_set_handler_impl(void *impl,
                                                  gateway_internal_handler_t handler,
                                                  void *user_data)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;
    if (!gw)
        return AIRY_EINVAL;

    /* 清理旧适配器 */
    if (gw->base.handler_adapter) {
        AIRY_FREE(gw->base.handler_adapter);
        gw->base.handler_adapter = NULL;
    }

    gw->base.handler = handler;
    gw->base.handler_data = user_data;

    return AIRY_SUCCESS;
}

/**
 * @brief HTTP/2 网关操作表
 */
static const gateway_ops_t http2_gateway_ops = {
    .start = http2_gateway_start_impl,
    .stop = http2_gateway_stop_impl,
    .destroy = http2_gateway_destroy_impl,
    .get_name = http2_gateway_get_name_impl,
    .get_stats = http2_gateway_get_stats_impl,
    .is_running = http2_gateway_is_running_impl,
    .set_handler = http2_gateway_set_handler_impl,
};

/* ========== 公共接口 ========== */

/**
 * @brief 初始化 CORS 配置（从环境变量读取）
 */
static void http2_init_cors_config(http_gateway_t *base)
{
    base->cors.allow_all_origins = false;
    base->cors.allowed_origins = NULL;
    base->cors.allowed_origins_count = 0;
    base->cors.allowed_methods = AIRY_STRDUP("POST, GET, OPTIONS");
    base->cors.allowed_headers = AIRY_STRDUP("Content-Type, Authorization");
    base->cors.max_age = 3600;

    const char *cors_mode = getenv("GATEWAY_CORS_MODE");
    if (cors_mode && strcmp(cors_mode, "dev") == 0) {
        base->cors.allow_all_origins = true;
    }

    const char *cors_origins = getenv("GATEWAY_CORS_ORIGINS");
    if (cors_origins && !base->cors.allow_all_origins) {
        char *origins_copy = AIRY_STRDUP(cors_origins);
        if (origins_copy) {
            size_t count = 1;
            for (char *p = origins_copy; *p; p++) {
                if (*p == ',')
                    count++;
            }

            if (count <= SIZE_MAX / sizeof(char *)) {
                base->cors.allowed_origins =
                    (char **)airy_malloc_array(count, sizeof(char *));
                if (base->cors.allowed_origins) {
                    char *saveptr = NULL;
                    char *token = strtok_r(origins_copy, ",", &saveptr);
                    size_t i = 0;
                    while (token && i < count) {
                        base->cors.allowed_origins[i++] = AIRY_STRDUP(token);
                        token = strtok_r(NULL, ",", &saveptr);
                    }
                    base->cors.allowed_origins_count = i;
                }
            }
            AIRY_FREE(origins_copy);
        }
    }
}

gateway_t *http2_gateway_create(const char *host, uint16_t port)
{
    if (!host) {
        return NULL;
    }

    http2_gateway_t *gw = AIRY_CALLOC(1, sizeof(http2_gateway_t));
    if (!gw) {
        return NULL;
    }

    /* 初始化 base (http_gateway_t) 字段 */
    http_gateway_t *base = &gw->base;
    base->daemon = NULL;
    base->port = port;
    base->host = AIRY_STRDUP(host);
    base->handler_adapter = NULL;
    base->handler = NULL;
    base->handler_data = NULL;

    if (!base->host) {
        AIRY_FREE(gw);
        return NULL;
    }

    atomic_init(&base->running, false);
    atomic_init(&base->requests_total, 0);
    atomic_init(&base->requests_failed, 0);
    atomic_init(&base->bytes_received, 0);
    atomic_init(&base->bytes_sent, 0);

    /* 最大请求体大小（默认 1MB） */
    base->max_request_size = 1 * 1024 * 1024;
    const char *env_max_size = getenv("GATEWAY_MAX_REQUEST_SIZE");
    if (env_max_size) {
        long size = strtol(env_max_size, NULL, 10);
        if (size > 0 && size <= 100 * 1024 * 1024) {
            base->max_request_size = (size_t)size;
        }
    }

    /* CORS 配置 */
    http2_init_cors_config(base);

    /* 速率限制器（默认禁用） */
    base->rate_limiter = NULL;
    const char *rate_limit_enabled = getenv("GATEWAY_RATE_LIMIT_ENABLED");
    if (rate_limit_enabled && strcmp(rate_limit_enabled, "true") == 0) {
        gateway_rate_limit_config_t rl_config;
        gateway_rate_limiter_get_default_config(&rl_config);
        rl_config.enabled = true;

        const char *rps = getenv("GATEWAY_RATE_LIMIT_RPS");
        if (rps) {
            rl_config.requests_per_second = (uint32_t)strtol(rps, NULL, 10);
        }

        base->rate_limiter = gateway_rate_limiter_create(&rl_config);
    }

    /* 多协议处理器 */
    base->protocol_handler = gateway_protocol_handler_create(NULL);

    /* 动态端点初始化 */
    base->dynamic_endpoints = NULL;
    base->dynamic_endpoint_count = 0;
    base->dynamic_endpoint_capacity = 0;

    /* 初始化 HTTP/2 特定字段 */
    gw->listen_fd = -1;
    gw->sessions = NULL;
    gw->session_count = 0;
    gw->session_capacity = 0;
    gw->event_thread = NULL;
    gw->max_concurrent_streams = HTTP2_DEFAULT_MAX_STREAMS;
    gw->connection_timeout = HTTP2_DEFAULT_TIMEOUT_SEC;

    /* 从环境变量读取配置 */
    const char *env_streams = getenv("GATEWAY_HTTP2_MAX_STREAMS");
    if (env_streams) {
        unsigned long v = strtoul(env_streams, NULL, 10);
        if (v > 0 && v <= 1000) {
            gw->max_concurrent_streams = (unsigned int)v;
        }
    }

    const char *env_timeout = getenv("GATEWAY_HTTP2_TIMEOUT");
    if (env_timeout) {
        unsigned long v = strtoul(env_timeout, NULL, 10);
        if (v > 0) {
            gw->connection_timeout = (unsigned int)v;
        }
    }

    atomic_init(&gw->running, false);

    /* 创建 gateway_t 包装 */
    gateway_t *gateway = AIRY_MALLOC(sizeof(gateway_t));
    if (!gateway) {
        if (base->host)
            AIRY_FREE(base->host);
        if (base->cors.allowed_methods)
            AIRY_FREE(base->cors.allowed_methods);
        if (base->cors.allowed_headers)
            AIRY_FREE(base->cors.allowed_headers);
        if (base->cors.allowed_origins) {
            for (size_t i = 0; i < base->cors.allowed_origins_count; i++)
                AIRY_FREE(base->cors.allowed_origins[i]);
            AIRY_FREE(base->cors.allowed_origins);
        }
        if (base->protocol_handler)
            gateway_protocol_handler_destroy(base->protocol_handler);
        if (base->rate_limiter)
            gateway_rate_limiter_destroy(base->rate_limiter);
        AIRY_FREE(gw);
        return NULL;
    }

    gateway->ops = &http2_gateway_ops;
    gateway->impl = gw;
    gateway->type = GATEWAY_TYPE_HTTP;
    gateway->public_handler = NULL;
    gateway->public_handler_data = NULL;

    LOG_INFO("HTTP/2 gateway created on %s:%u", host, port);

    return gateway;
}

int http2_gateway_start(http2_gateway_t *gw)
{
    if (!gw)
        return AIRY_EINVAL;
    return http2_gateway_start_impl(gw);
}

int http2_gateway_stop(http2_gateway_t *gw)
{
    if (!gw)
        return AIRY_EINVAL;
    http2_gateway_stop_impl(gw);
    return AIRY_SUCCESS;
}

#endif /* AIRY_HAS_HTTP2 */

/* ========== 无 nghttp2 时的桩实现 ========== */

#ifndef AIRY_HAS_HTTP2

gateway_t *http2_gateway_create(const char *host __attribute__((unused)),
                                uint16_t port __attribute__((unused)))
{
    LOG_WARN("HTTP/2 gateway not available: nghttp2 not compiled in");
    return NULL;
}

int http2_gateway_start(http2_gateway_t *gw __attribute__((unused)))
{
    return AIRY_ENOSYS;
}

int http2_gateway_stop(http2_gateway_t *gw __attribute__((unused)))
{
    return AIRY_ENOSYS;
}

#endif /* !AIRY_HAS_HTTP2 */
