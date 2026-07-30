/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file http2_gateway.h
 * @brief HTTP/2 网关接口 — 基于 nghttp2 的 HTTP/2 服务器
 *
 * 在 http_gateway_t 基础上扩展 HTTP/2 协议支持，复用 JSON-RPC
 * 路由逻辑和 gateway_protocol_handler 多协议处理器。
 *
 * 设计原则：
 *   IRON-2 铁律：禁止桩函数和简化功能，必须实现真正可用的 HTTP/2 服务器
 *   K-1 内核极简：只做协议转换，零业务逻辑
 *   S-2 层次分解：每层职责单一，易于测试和维护
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

// @owner: team-B
#ifndef AIRY_RT_GATEWAY_HTTP2_H
#define AIRY_RT_GATEWAY_HTTP2_H

#include "gateway_internal.h"
#include "http_gateway.h"

#include <nghttp2/nghttp2.h>

#include "atomic_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP/2 流上下文
 *
 * 每个 HTTP/2 流（stream）对应一个请求上下文，
 * 存储请求头、请求体和响应数据。
 */
typedef struct http2_stream_context {
    int32_t stream_id;       /**< HTTP/2 流 ID */
    char *method;            /**< HTTP 方法（:method 伪头） */
    char *path;              /**< 请求路径（:path 伪头） */
    char *content_type;      /**< Content-Type 头 */
    char *origin;            /**< Origin 头（用于 CORS） */

    uint8_t *request_body;   /**< 请求体缓冲区 */
    size_t request_body_len; /**< 请求体已用长度 */
    size_t request_body_cap; /**< 请求体缓冲区容量 */

    char *response_body;      /**< 响应体（由 handle_jsonrpc_request 生成） */
    size_t response_body_len; /**< 响应体长度 */
    size_t response_sent;     /**< 已发送响应体偏移 */
    int response_status;      /**< HTTP 状态码 */

    bool headers_complete; /**< HEADERS 帧是否已完整接收 */
    bool body_complete;    /**< 请求体是否已完整接收（END_STREAM） */
    bool response_sent_flag; /**< 响应是否已提交 */
} http2_stream_context_t;

/**
 * @brief HTTP/2 会话（每连接）
 *
 * 每个接受的 TCP 连接对应一个 nghttp2 服务端会话。
 */
typedef struct http2_gateway_session {
    int fd;                     /**< TCP socket fd */
    nghttp2_session *session;   /**< nghttp2 会话 */
    struct http2_gateway *gateway; /**< 反向引用网关实例 */
    uint64_t connect_time_ns;   /**< 连接建立时间 */
    uint64_t last_activity_ns;  /**< 最后活动时间 */
    bool closing;               /**< 标记为关闭中 */

    /* P0 修复: 部分写入缓冲区。
     * nghttp2_session_mem_send() 返回数据后认为已被消费，
     * 但 write() 可能只写入部分字节。剩余数据必须缓存到下次 POLLOUT。 */
    uint8_t *pending_send_buf;   /**< 未发送完毕的数据缓冲区 */
    size_t pending_send_len;     /**< 缓冲区中数据总长度 */
    size_t pending_send_offset;  /**< 已写入偏移量 */
} http2_gateway_session_t;

/**
 * @brief HTTP/2 gateway 扩展结构
 *
 * 在 http_gateway_t 基础上增加 HTTP/2 会话管理。
 * base 字段作为第一个成员，支持向 http_gateway_t* 的安全转换。
 */
typedef struct http2_gateway {
    http_gateway_t base;              /**< 继承 HTTP/1.1 基础功能 */
    int listen_fd;                    /**< HTTP/2 TCP 监听 socket */
    http2_gateway_session_t **sessions; /**< 活跃会话数组 */
    size_t session_count;             /**< 当前会话数 */
    size_t session_capacity;          /**< 会话数组容量 */
    atomic_bool running;              /**< 事件循环运行标志 */
    void *event_thread;               /**< 事件循环线程句柄 (pthread_t*) */
    unsigned int max_concurrent_streams; /**< 最大并发流 */
    unsigned int connection_timeout;  /**< 连接空闲超时（秒） */
} http2_gateway_t;

/**
 * @brief 创建 HTTP/2 gateway
 *
 * 创建 HTTP/2 网关实例，复用 HTTP/1.1 的路由逻辑和协议处理器。
 * 创建后通过 gateway_start() 启动，gateway_destroy() 销毁。
 *
 * @param host 监听地址（如 "127.0.0.1", "0.0.0.0"）
 * @param port 监听端口
 * @return gateway 实例，失败返回 NULL
 *
 * @ownership 调用者需通过 gateway_destroy() 释放
 */
gateway_t *http2_gateway_create(const char *host, uint16_t port);

/**
 * @brief 启动 HTTP/2 gateway
 *
 * 创建监听 socket 并启动事件循环线程。
 *
 * @param gw HTTP/2 网关实例
 * @return AIRY_SUCCESS 成功，负数错误码失败
 */
int http2_gateway_start(http2_gateway_t *gw);

/**
 * @brief 停止 HTTP/2 gateway
 *
 * 设置运行标志为 false，关闭监听 socket，等待事件循环线程退出。
 *
 * @param gw HTTP/2 网关实例
 * @return AIRY_SUCCESS 成功
 */
int http2_gateway_stop(http2_gateway_t *gw);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_HTTP2_H */
