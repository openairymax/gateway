/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file ws_gateway.c
 * @brief WebSocket网关实现 - libwebsockets集成
 *
 * 实现WebSocket双向通信协议，通过系统调用接口与内核通信。
 * 网关层只负责协议转换，不包含业务逻辑。
 *
 * 设计原则：
 *   K-1 内核极简：只做协议转换，零业务逻辑
 *   S-2 层次分解：每层职责单一，易于测试和维护
 *   E-8 可测试性：路由处理函数独立可测试
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

// @owner: team-B
#include "ws_gateway.h"

#include "../utils/gateway_rpc_handler.h"
#include "../utils/gateway_utils.h"
#include "../utils/jsonrpc.h"
#include "../utils/syscall_router.h"

#ifdef GATEWAY_HAS_WS

#include <cjson/cJSON.h>
/* P0.18.2: 引入 cjson_helpers.h 提供 CJSON_PARSE_GUARD/CJSON_AUTO_FREE 宏 */
#include <cjson_helpers.h>
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 跨平台原子操作支持 - 使用统一的 atomic_compat.h */
#include "atomic_compat.h"

/* 平台特定头文件 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

#include "airy_memory.h"
#include "error.h"

#ifndef _WIN32
#include <pthread.h>
#endif

/* ========== 前向声明 ========== */

struct ws_gateway;
typedef struct ws_gateway ws_gateway_t;

/* ========== WebSocket协议定义 ========== */

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                       size_t len);

static const struct lws_protocols ws_protocols[] = {{
                                                        "agentrt-rpc",
                                                        ws_callback,
                                                        sizeof(void *),
                                                        4096,
                                                        0,
                                                        NULL,
                                                        0,
                                                    },
                                                    {NULL, NULL, 0, 0, 0, NULL, 0}};

/* ========== WebSocket网关内部结构 ========== */

/**
 * @brief WebSocket连接上下文
 */
typedef struct ws_connection_context {
    struct lws *wsi;           /**< WebSocket实例 */
    char *session_id;          /**< 会话ID */
    char *remote_addr;         /**< 远程地址 */
    uint64_t connect_time_ns;  /**< 连接时间 */
    uint64_t last_activity_ns; /**< 最后活动时间 */

    size_t messages_sent;     /**< 发送消息数 */
    size_t messages_received; /**< 接收消息数 */
    size_t bytes_sent;        /**< 发送字节数 */
    size_t bytes_received;    /**< 接收字节数 */
} ws_connection_context_t;

/**
 * @brief WebSocket网关内部结构
 */
struct ws_gateway {
    struct lws_context *context; /**< LWS上下文 */
    uint16_t port;               /**< 监听端口 */
    char *host;                  /**< 监听地址 */

    void *handler_adapter;              /**< 公共回调适配器（动态分配） */
    gateway_internal_handler_t handler; /**< 内部请求处理回调 */
    void *handler_data;                 /**< 回调用户数据 */

    atomic_bool running; /**< 运行标志 */

    atomic_uint_fast64_t connections_total;  /**< 总连接数 */
    atomic_uint_fast64_t connections_active; /**< 活跃连接数 */
    atomic_uint_fast64_t messages_total;     /**< 总消息数 */
    atomic_uint_fast64_t bytes_sent;         /**< 发送字节数 */
    atomic_uint_fast64_t bytes_received;     /**< 接收字节数 */

    size_t max_request_size; /**< 最大请求大小 */
    void *event_thread;      /**< libwebsockets 事件循环线程句柄（pthread_t*，POSIX） */
};

/* ========== 消息协议定义 ========== */

/**
 * @brief WebSocket消息类型
 */
typedef enum {
    WS_MSG_TYPE_PING = 1,     /**< Ping消息 */
    WS_MSG_TYPE_PONG,         /**< Pong消息 */
    WS_MSG_TYPE_RPC_REQUEST,  /**< RPC请求 */
    WS_MSG_TYPE_RPC_RESPONSE, /**< RPC响应 */
    WS_MSG_TYPE_NOTIFICATION, /**< 通知消息 */
    WS_MSG_TYPE_ERROR         /**< 错误消息 */
} ws_message_type_t;

/**
 * @brief WebSocket消息结构
 */
typedef struct ws_message {
    ws_message_type_t type; /**< 消息类型 */
    char *session_id;       /**< 会话ID */
    cJSON *payload;         /**< 消息载荷 */
    uint64_t timestamp_ns;  /**< 时间戳 */
} ws_message_t;

/* ========== 辅助函数 ========== */

/**
 * @brief 创建WebSocket消息
 * @param type 消息类型
 * @param session_id 会话ID（可为NULL）
 * @param payload 消息载荷（可为NULL）
 * @return 消息结构指针，失败返回NULL
 */
static ws_message_t *ws_message_create(ws_message_type_t type, const char *session_id,
                                       cJSON *payload)
{
    ws_message_t *msg = AIRY_CALLOC(1, sizeof(ws_message_t));
    if (!msg)
        return NULL;

    msg->type = type;
    msg->session_id = session_id ? AIRY_STRDUP(session_id) : NULL;
    msg->payload = payload ? cJSON_Duplicate(payload, 1) : NULL;
    msg->timestamp_ns = gateway_time_ns();

    return msg;
}

/**
 * @brief 销毁WebSocket消息
 * @param msg 消息结构指针
 */
static void ws_message_destroy(ws_message_t *msg)
{
    if (!msg)
        return;

    if (msg->session_id)
        AIRY_FREE(msg->session_id);
    if (msg->payload)
        cJSON_Delete(msg->payload);
    AIRY_FREE(msg);
}

/**
 * @brief 序列化WebSocket消息为JSON字符串
 * @param msg 消息结构指针
 * @return JSON字符串，需调用者AIRY_FREE()
 */
static char *ws_message_to_json(ws_message_t *msg)
{
    cJSON *json = cJSON_CreateObject();
    if (!json)
        return NULL;

    const char *type_str = NULL;
    switch (msg->type) {
    case WS_MSG_TYPE_PING:
        type_str = "ping";
        break;
    case WS_MSG_TYPE_PONG:
        type_str = "pong";
        break;
    case WS_MSG_TYPE_RPC_REQUEST:
        type_str = "rpc_request";
        break;
    case WS_MSG_TYPE_RPC_RESPONSE:
        type_str = "rpc_response";
        break;
    case WS_MSG_TYPE_NOTIFICATION:
        type_str = "notification";
        break;
    case WS_MSG_TYPE_ERROR:
        type_str = "error";
        break;
    }
    cJSON_AddStringToObject(json, "type", type_str ? type_str : "unknown");

    if (msg->session_id) {
        cJSON_AddStringToObject(json, "session_id", msg->session_id);
    }

    cJSON_AddNumberToObject(json, "timestamp", msg->timestamp_ns / 1000000000.0);

    if (msg->payload) {
        cJSON_AddItemToObject(json, "payload", cJSON_Duplicate(msg->payload, 1));
    }

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    return json_str;
}

/**
 * @brief 发送WebSocket消息
 * @param wsi WebSocket实例
 * @param msg 消息结构指针
 * @return 成功返回发送字节数，失败返回-1
 */
static int ws_send_message(struct lws *wsi, ws_message_t *msg)
{
    if (!wsi || !msg) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                              "ws_send_message: IO error");
        return AIRY_ERR_UNKNOWN;
    }

    char *json_str = ws_message_to_json(msg);
    if (!json_str) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "if: failed");
        return AIRY_ERR_UNKNOWN;
    }

    size_t out_len = strlen(json_str);
    int result = lws_write(wsi, (unsigned char *)json_str, out_len, LWS_WRITE_TEXT);

    AIRY_FREE(json_str);
    return result;
}

/* ========== RPC处理（使用统一处理器） ========== */

static int ws_rpc_handler_adapter(const char *request_json, char **response_json, void *ctx)
{
    ws_gateway_t *gw = (ws_gateway_t *)ctx;
    if (!gw || !gw->handler) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                              "ws_rpc_handler_adapter: failed");
        return AIRY_ERR_UNKNOWN;
    }
    char *result = gw->handler((void *)request_json, gw->handler_data);
    if (!result) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "if: failed");
        return AIRY_ERR_UNKNOWN;
    }
    *response_json = result;
    return 0;
}

/**
 * @brief 处理RPC请求（使用统一RPC处理器）
 *
 * 通过 gateway_rpc_handle_request() 实现统一的请求处理流程，
 * 消除与 HTTP/Stdio 网关的代码重复。
 *
 * @param gateway 网关实例
 * @param request JSON-RPC请求对象
 * @return JSON响应字符串，需调用者AIRY_FREE()
 */
static char *handle_rpc_request(ws_gateway_t *gateway, cJSON *request)
{
    if (!gateway || !request) {
        return jsonrpc_create_error_response(NULL, -32600, "Invalid request", NULL);
    }

    rpc_result_t result = gateway_rpc_handle_request(request, ws_rpc_handler_adapter, gateway);

    if (result.error_code != 0 || !result.response_json) {
        char *error_resp =
            result.response_json
                ? result.response_json
                : jsonrpc_create_error_response(NULL, -32603, "Internal error", NULL);
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

/* ========== 路由处理函数（降低圈复杂度） ========== */

/**
 * @brief 处理连接建立
 * @param gateway 网关实例
 * @param context 连接上下文
 * @param user 用户指针
 * @return 成功返回0，失败返回-1
 */
static int handle_ws_established(ws_gateway_t *gateway, ws_connection_context_t **context_ptr,
                                 void **user)
{
    ws_connection_context_t *context = AIRY_CALLOC(1, sizeof(ws_connection_context_t));
    if (!context) {
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                              "handle_ws_established: allocation failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    context->wsi = (struct lws *)*user;
    context->connect_time_ns = gateway_time_ns();
    context->last_activity_ns = gateway_time_ns();

    *context_ptr = context;
    *user = context;

    atomic_fetch_add(&gateway->connections_total, 1);
    atomic_fetch_add(&gateway->connections_active, 1);

    return 0;
}

/**
 * @brief 处理Ping消息
 * @param context 连接上下文
 * @param wsi WebSocket实例
 * @return 成功返回0
 */
static int handle_ws_ping(ws_connection_context_t *context, struct lws *wsi)
{
    ws_message_t *pong_msg = ws_message_create(WS_MSG_TYPE_PONG, context->session_id, NULL);
    if (pong_msg) {
        ws_send_message(wsi, pong_msg);
        ws_message_destroy(pong_msg);
    }
    return 0;
}

/**
 * @brief 处理RPC请求消息
 * @param gateway 网关实例
 * @param context 连接上下文
 * @param rpc_request RPC请求对象
 * @param wsi WebSocket实例
 * @return 成功返回0
 */
static int handle_ws_rpc_request(ws_gateway_t *gateway, ws_connection_context_t *context,
                                 cJSON *rpc_request, struct lws *wsi)
{
    char *response = handle_rpc_request(gateway, rpc_request);
    if (!response) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                              "handle_ws_rpc_request: IO error");
        return AIRY_ERR_UNKNOWN;
    }

    /* P0.18.2: 模式 A — CJSON_PARSE_GUARD 自动释放 + NULL 检查 */
    CJSON_PARSE_GUARD(response_json, response, {
        AIRY_FREE(response);
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                              "cJSON_Parse: parse error");
        return AIRY_ERR_UNKNOWN;
    });

    ws_message_t *response_msg =
        ws_message_create(WS_MSG_TYPE_RPC_RESPONSE, context->session_id, response_json);
    /* response_json 由 CJSON_AUTO_FREE 自动释放 */

    if (response_msg) {
        ws_send_message(wsi, response_msg);
        ws_message_destroy(response_msg);
    }

    AIRY_FREE(response);
    return 0;
}

/**
 * @brief 处理未知消息类型
 * @param wsi WebSocket实例
 * @param unknown_type 未知类型字符串
 * @return 成功返回0
 */
static int handle_ws_unknown_message(struct lws *wsi, const char *unknown_type)
{
    char err_buf[128];
    snprintf(err_buf, sizeof(err_buf), "Unknown message type: %s",
             unknown_type ? unknown_type : "null");

    char *error_json = jsonrpc_create_error_response(NULL, -32600, err_buf, NULL);
    if (!error_json) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                              "jsonrpc_create_error_response returned NULL");
        return AIRY_ERR_UNKNOWN;
    }

    ws_message_t *error_msg = ws_message_create(WS_MSG_TYPE_ERROR, NULL, NULL);
    if (error_msg) {
        cJSON *payload = cJSON_CreateObject();
        if (payload) {
            cJSON_AddStringToObject(payload, "error", err_buf);
            error_msg->payload = payload;
        }
        ws_send_message(wsi, error_msg);
        ws_message_destroy(error_msg);
    }

    AIRY_FREE(error_json);
    return 0;
}

/**
 * @brief 处理连接关闭
 * @param gateway 网关实例
 * @param context_ptr 连接上下文指针
 * @return 成功返回0
 */
static int handle_ws_closed(ws_gateway_t *gateway, ws_connection_context_t **context_ptr)
{
    ws_connection_context_t *context = *context_ptr;
    if (!context)
        return 0;

    atomic_fetch_sub(&gateway->connections_active, 1);

    if (context->session_id)
        AIRY_FREE(context->session_id);
    if (context->remote_addr)
        AIRY_FREE(context->remote_addr);
    AIRY_FREE(context);

    *context_ptr = NULL;

    return 0;
}

/* ========== WebSocket回调函数（重构后版本） ========== */

/**
 * @brief WebSocket回调函数
 *
 * 采用路由模式，将不同reason的处理分离到独立函数，
 * 大幅降低圈复杂度，提高可测试性。
 *
 * @param wsi WebSocket实例
 * @param reason 回调原因
 * @param user 用户指针
 * @param in 输入数据
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                       size_t len)
{
    ws_gateway_t *gateway = (ws_gateway_t *)lws_context_user(lws_get_context(wsi));
    ws_connection_context_t *context = NULL;

    /* 连接建立前的回调（如 LWS_CALLBACK_PROTOCOL_INIT/DESTROY，在
     * lws_create_context 期间触发）user 为 NULL，解引用会段错误；
     * 仅对已建立连接的会话回调解析 per_session_data。 */
    if (user)
        context = (ws_connection_context_t *)*(void **)user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        return handle_ws_established(gateway, &context, &user);

    case LWS_CALLBACK_RECEIVE:
        if (!context) {
            airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                                  "handle_ws_established: failed");
            return AIRY_ERR_UNKNOWN;
        }

        if (len > gateway->max_request_size) {
            char *error_json =
                jsonrpc_create_error_response(NULL, -32603, "Message too large", NULL);
            if (error_json) {
                ws_message_t *error_msg =
                    ws_message_create(WS_MSG_TYPE_ERROR, NULL, cJSON_Parse(error_json));
                if (error_msg) {
                    ws_send_message(wsi, error_msg);
                    ws_message_destroy(error_msg);
                }
                AIRY_FREE(error_json);
            }
            airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                                  "ws_send_message: IO error");
            return AIRY_ERR_UNKNOWN;
        }

        /* 更新统计信息 */
        context->last_activity_ns = gateway_time_ns();
        context->messages_received++;
        context->bytes_received += len;
        atomic_fetch_add(&gateway->messages_total, 1);
        atomic_fetch_add(&gateway->bytes_received, len);

        /* 解析JSON消息 */
        /* P0: lws 的 in 缓冲区不保证 '\0' 结尾，cJSON_Parse 按 NUL 扫描会越界读；
         * 改用 cJSON_ParseWithLength 按 len 解析（CJSON_AUTO_FREE 保留自动释放） */
        CJSON_AUTO_FREE cJSON *json = cJSON_ParseWithLength((const char *)in, len);
        if (!json) {
            /* 解析失败，发送错误响应 */
            char *error_json = jsonrpc_create_error_response(NULL, -32700, "Parse error", NULL);
            if (error_json) {
                ws_message_t *error_msg =
                    ws_message_create(WS_MSG_TYPE_ERROR, NULL, cJSON_Parse(error_json));
                if (error_msg) {
                    ws_send_message(wsi, error_msg);
                    ws_message_destroy(error_msg);
                }
                AIRY_FREE(error_json);
            }
            return 0;
        }

        /* 提取消息类型 */
        cJSON *type = cJSON_GetObjectItem(json, "type");
        if (!type || !cJSON_IsString(type)) {
            /* json 由 CJSON_AUTO_FREE 自动释放 */
            return handle_ws_unknown_message(wsi, "missing type field");
        }

        /* 根据消息类型路由处理 */
        const char *type_str = type->valuestring;
        int result = 0;

        if (strcmp(type_str, "ping") == 0) {
            result = handle_ws_ping(context, wsi);
        } else if (strcmp(type_str, "rpc_request") == 0) {
            cJSON *rpc_request = cJSON_GetObjectItem(json, "payload");
            if (rpc_request) {
                result = handle_ws_rpc_request(gateway, context, rpc_request, wsi);
            }
        } else {
            result = handle_ws_unknown_message(wsi, type_str);
        }

        /* json 由 CJSON_AUTO_FREE 自动释放 */
        return result;

    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
    case LWS_CALLBACK_CLOSED_HTTP:
        return handle_ws_closed(gateway, &context);

    default:
        break;
    }

    return 0;
}

/* ========== 网关操作表 ========== */

#ifndef _WIN32
/**
 * @brief libwebsockets 事件循环线程
 *
 * lws_create_context 只创建上下文，必须持续调用 lws_service 驱动
 * 连接收发（IRON-2：真实可用的 WebSocket 服务器，非桩）。
 * 50ms 超时便于 running=false 后及时退出并 join。
 */
static void *ws_gateway_event_loop(void *arg)
{
    ws_gateway_t *gateway = (ws_gateway_t *)arg;
    while (gateway && atomic_load(&gateway->running)) {
        lws_service(gateway->context, 50);
    }
    return NULL;
}
#endif

static airy_err_t ws_gateway_start(void *gateway_impl)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;

    struct lws_context_creation_info info;
    AIRY_MEMSET(&info, 0, sizeof(info));
    info.port = gateway->port;
    info.iface = gateway->host;
    info.protocols = ws_protocols;
    info.user = gateway;

    gateway->context = lws_create_context(&info);
    if (!gateway->context) {
        return AIRY_EBUSY;
    }

    atomic_store(&gateway->running, true);

#ifndef _WIN32
    /* 启动事件循环线程驱动 lws_service */
    pthread_t *thread = (pthread_t *)AIRY_MALLOC(sizeof(pthread_t));
    if (!thread) {
        atomic_store(&gateway->running, false);
        lws_context_destroy(gateway->context);
        gateway->context = NULL;
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    if (pthread_create(thread, NULL, ws_gateway_event_loop, gateway) != 0) {
        AIRY_FREE(thread);
        atomic_store(&gateway->running, false);
        lws_context_destroy(gateway->context);
        gateway->context = NULL;
        return AIRY_EBUSY;
    }
    gateway->event_thread = thread;
#else
    /* Windows：事件循环由外部 lws_service 驱动 */
    gateway->event_thread = NULL;
#endif

    return AIRY_SUCCESS;
}

static void ws_gateway_stop(void *gateway_impl)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;

    atomic_store(&gateway->running, false);

#ifndef _WIN32
    if (gateway->event_thread) {
        /* lws_cancel_service 可从其他线程安全唤醒 lws_service 事件循环，
         * 使 event thread 在 running=false 后立即退出。若不唤醒，事件循环
         * 依赖下一次 poll 超时（50ms）返回，在 netlink 事件风暴等场景下
         * lws_service 可能长时间处理事件，导致 stop 的 join 阻塞数秒，
         * 使守护进程优雅退出超过外部停止阈值而被强制 KILL。 */
        if (gateway->context) {
            lws_cancel_service(gateway->context);
        }
        pthread_join(*(pthread_t *)gateway->event_thread, NULL);
        AIRY_FREE(gateway->event_thread);
        gateway->event_thread = NULL;
    }
#endif

    if (gateway->context) {
        lws_context_destroy(gateway->context);
        gateway->context = NULL;
    }
}

static void ws_gateway_destroy(void *gateway_impl)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;

    ws_gateway_stop(gateway);

    if (gateway->handler_adapter) {
        AIRY_FREE(gateway->handler_adapter);
        gateway->handler_adapter = NULL;
    }
    gateway->handler = NULL;
    gateway->handler_data = NULL;

    if (gateway->host) {
        AIRY_FREE(gateway->host);
    }

    AIRY_FREE(gateway);
}

static const char *ws_gateway_get_name(void *gateway_impl __attribute__((unused)))
{
    return "WebSocket Gateway";
}

static bool ws_gateway_is_running(void *gateway_impl)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;
    if (!gateway)
        return false;
    return atomic_load(&gateway->running);
}

static airy_err_t ws_gateway_get_stats(void *gateway_impl, char **out_json)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;
    if (!gateway || !out_json)
        return AIRY_EINVAL;

    cJSON *stats = cJSON_CreateObject();
    if (!stats)
        return AIRY_ENOMEM;

    cJSON_AddNumberToObject(stats, "connections_total",
                            (double)atomic_load(&gateway->connections_total));
    cJSON_AddNumberToObject(stats, "connections_active",
                            (double)atomic_load(&gateway->connections_active));
    cJSON_AddNumberToObject(stats, "messages_total", (double)atomic_load(&gateway->messages_total));
    cJSON_AddNumberToObject(stats, "bytes_sent", (double)atomic_load(&gateway->bytes_sent));
    cJSON_AddNumberToObject(stats, "bytes_received", (double)atomic_load(&gateway->bytes_received));

    char *json_str = cJSON_Print(stats);
    cJSON_Delete(stats);

    if (!json_str)
        return AIRY_ENOMEM;

    *out_json = json_str;
    return AIRY_SUCCESS;
}

static airy_err_t ws_gateway_set_handler(void *gateway_impl,
                                              gateway_internal_handler_t handler, void *user_data)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;
    if (!gateway)
        return AIRY_EINVAL;

    if (gateway->handler_adapter) {
        AIRY_FREE(gateway->handler_adapter);
        gateway->handler_adapter = NULL;
    }

    gateway->handler = handler;
    gateway->handler_data = user_data;

    return AIRY_SUCCESS;
}

static const gateway_ops_t ws_gateway_ops = {.start = ws_gateway_start,
                                             .stop = ws_gateway_stop,
                                             .destroy = ws_gateway_destroy,
                                             .get_name = ws_gateway_get_name,
                                             .get_stats = ws_gateway_get_stats,
                                             .is_running = ws_gateway_is_running,
                                             .set_handler = ws_gateway_set_handler};

/* ========== 公共接口 ========== */

/**
 * @brief 创建WebSocket网关实例
 * @param host 监听地址（如 "127.0.0.1", "0.0.0.0"），不能为NULL
 * @param port 监听端口（如 8081）
 * @return 网关句柄，失败返回NULL（内存不足或参数无效）
 *
 * @ownership 调用者必须通过 gateway_destroy() 释放
 * @threadsafe 安全
 * @since 1.0.0
 */
gateway_t *ws_gateway_create(const char *host, uint16_t port)
{
    if (!host) {
        return NULL;
    }

    ws_gateway_t *gateway = AIRY_CALLOC(1, sizeof(ws_gateway_t));
    if (!gateway) {
        return NULL;
    }

    gateway->port = port;
    gateway->host = AIRY_STRDUP(host);
    gateway->handler_adapter = NULL;
    gateway->handler = NULL;
    gateway->handler_data = NULL;

    if (!gateway->host) {
        AIRY_FREE(gateway);
        return NULL;
    }

    atomic_init(&gateway->running, false);
    atomic_init(&gateway->connections_total, 0);
    atomic_init(&gateway->connections_active, 0);
    atomic_init(&gateway->messages_total, 0);
    atomic_init(&gateway->bytes_sent, 0);
    atomic_init(&gateway->bytes_received, 0);

    gateway->max_request_size = 10 * 1024 * 1024; /* 10MB */

    gateway_t *gw = AIRY_MALLOC(sizeof(gateway_t));
    if (!gw) {
        AIRY_FREE(gateway->host);
        AIRY_FREE(gateway);
        return NULL;
    }

    gw->ops = &ws_gateway_ops;
    gw->impl = gateway;
    gw->type = GATEWAY_TYPE_WS;
    gw->public_handler = NULL;
    gw->public_handler_data = NULL;

    return gw;
}

#endif /* GATEWAY_HAS_WS */

#ifndef GATEWAY_HAS_WS

gateway_t *ws_gateway_create(const char *host __attribute__((unused)),
                             uint16_t port __attribute__((unused)))
{
    return NULL;
}

#endif /* !GATEWAY_HAS_WS */
