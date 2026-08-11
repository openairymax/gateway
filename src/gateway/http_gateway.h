/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 *
 * @file http_gateway.h
 * @brief HTTP网关接口
 *
 */

/* @owner: team-B */
#ifndef AIRY_RT_GATEWAY_HTTP_H
#define AIRY_RT_GATEWAY_HTTP_H

#include "gateway_internal.h"

#include <stdint.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#else
struct cJSON;
typedef struct cJSON cJSON;
#endif


#include "atomic_compat.h"


struct gateway_rate_limiter;
typedef struct gateway_rate_limiter gateway_rate_limiter_t;
struct gateway_protocol_handler_s;
typedef struct gateway_protocol_handler_s *gateway_protocol_handler_t;

#ifdef __cplusplus
extern "C" {
#endif

struct MHD_Connection;
struct MHD_Response;

/**
 * @brief CORS 配置结构
 *
 * 用于控制跨源资源共享(CORS)的安全设置。
 * 生产环境应配置白名单，开发环境可允许所有来源。
 */
typedef struct {
    bool allow_all_origins;
    char **allowed_origins;
    size_t allowed_origins_count;
    char *allowed_methods;
    char *allowed_headers;
    int max_age;
} cors_config_t;

typedef struct http_request_context {
    const char *method;
    const char *url;
    const char *upload_data;
    size_t upload_data_size;

    cJSON *json_request;
    uint64_t start_time_ns;
} http_request_context_t;

typedef struct {
    char *method;
    char *path;
    gateway_endpoint_handler_t handler;
    void *user_data;
} http_dynamic_endpoint_t;

typedef struct http_gateway {
    struct MHD_Daemon *daemon;
    uint16_t port;
    char *host;

    void *handler_adapter;
    gateway_internal_handler_t handler;
    void *handler_data;
    atomic_bool running;
    atomic_uint_fast64_t requests_total;
    atomic_uint_fast64_t requests_failed;
    atomic_uint_fast64_t bytes_received;
    atomic_uint_fast64_t bytes_sent;
    size_t max_request_size;
    unsigned int connection_limit;
    unsigned int connection_timeout;
    cors_config_t cors;
    gateway_rate_limiter_t *rate_limiter;
    gateway_protocol_handler_t protocol_handler;
    http_dynamic_endpoint_t *dynamic_endpoints;
    size_t dynamic_endpoint_count;
    size_t dynamic_endpoint_capacity;
} http_gateway_t;

/**
 * @brief 创建HTTP网关
 *
 * @param host 监听地址
 * @param port 监听端口
 * @return 网关实例，失败返回NULL
 *
 * @ownership 调用者需通过gateway_destroy()释放
 */
gateway_t *http_gateway_create(const char *host, uint16_t port);

/**
 * @brief 处理JSON-RPC请求
 */
char *handle_jsonrpc_request(http_gateway_t *gateway, http_request_context_t *context);

/**
 * @brief 创建HTTP响应
 */
struct MHD_Response *create_http_response(int status_code, const char *content, size_t content_len);

/**
 * @brief 创建HTTP响应（安全CORS版本）
 *
 * 根据网关CORS配置自动设置CORS响应头，所有路由处理函数应优先使用此函数。
 *
 * @param gateway HTTP网关实例（用于CORS配置）
 * @param connection MHD连接对象（用于获取Origin头）
 * @param status_code HTTP状态码（当前未使用，保留供未来扩展）
 * @param content 响应内容
 * @param content_len 内容长度
 * @return MHD响应对象
 */
struct MHD_Response *create_http_response_ex(http_gateway_t *gateway,
                                             struct MHD_Connection *connection, int status_code,
                                             const char *content, size_t content_len);

/**
 * @brief HTTP请求处理回调函数类型
 */
typedef int (*http_request_handler_t)(void *cls, struct MHD_Connection *connection, const char *url,
                                      const char *method, const char *version,
                                      const char *upload_data, size_t *upload_data_size,
                                      void **con_cls);

/**
 * @brief HTTP请求处理函数
 */
int handle_http_request(void *cls, struct MHD_Connection *connection, const char *url,
                        const char *method, const char *version, const char *upload_data,
                        size_t *upload_data_size, void **con_cls);

/**
 * @brief 应用安全HTTP响应头
 *
 * 添加安全相关的HTTP头，如X-Content-Type-Options, X-Frame-Options等
 *
 * @param response MHD响应对象
 */
void gateway_apply_security_headers(struct MHD_Response *response);

/**
 * @brief 应用CORS响应头
 *
 * 根据网关CORS配置和请求Origin头，自动设置CORS响应头。
 * 应在所有直接创建MHD_Response的地方调用（配合 gateway_apply_security_headers）。
 *
 * @param gateway HTTP网关实例
 * @param connection MHD连接对象
 * @param response MHD响应对象
 */
void gateway_apply_cors_headers(http_gateway_t *gateway, struct MHD_Connection *connection,
                                struct MHD_Response *response);

/**
 * @brief 解析JSON请求体
 *
 * @param gateway HTTP网关实例
 * @param context 请求上下文
 * @param data 请求体数据
 * @param size 数据大小
 * @return 0成功，非0失败
 */
int parse_json_request(http_gateway_t *gateway, http_request_context_t *context, const char *data,
                       size_t size);

/**
 * @brief 注册动态端点到HTTP网关
 *
 * @param gateway HTTP网关实例
 * @param method HTTP方法（将内部复制）
 * @param path URL路径（将内部复制）
 * @param handler 端点处理回调
 * @param user_data 传递给回调的用户数据
 * @return 0 成功，-1 参数无效，-2 内存不足
 */
int http_gateway_register_endpoint(http_gateway_t *gateway, const char *method, const char *path,
                                   gateway_endpoint_handler_t handler, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_HTTP_H */
