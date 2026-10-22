// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file ws_gateway_callback.c
 * @brief WebSocket gateway - libwebsockets event callback dispatch.
 *
 * Implements the libwebsockets protocol table and the single event
 * callback that routes each reason to its dedicated handler, single
 * responsibility. Split out of ws_gateway.c.
 */

// @owner: team-B
#include "ws_gateway_internal.h"

#include "gateway_utils.h"
#include "jsonrpc_helpers.h"

#include "airy_memory.h"
#include "error.h"
#include "logging.h"

#ifdef GATEWAY_HAS_WS

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <string.h>

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                       size_t len);

const struct lws_protocols ws_protocols[] = {{
                                                        "agentrt-rpc",
                                                        ws_callback,
                                                        sizeof(void *),
                                                        4096,
                                                        0,
                                                        NULL,
                                                        0,
                                                    },
                                                    {NULL, NULL, 0, 0, 0, NULL, 0}};

/* 统一错误回执：构造 JSON-RPC 错误对象 → 解析 → 深拷贝进消息 → 发送。
 * cJSON_Parse 的结果由 ws_message_create 深拷贝，解析树与字符串都需就地释放；
 * 三条错误路径共用本件，避免重复代码与漏释放。 */
static void ws_err_reply(struct lws *wsi, int code, const char *msg)
{
    char *error_json = jsonrpc_create_error_response(NULL, code, msg, NULL);
    if (!error_json)
        return;

    cJSON *err_parsed = cJSON_Parse(error_json);
    ws_message_t *error_msg =
        err_parsed ? ws_message_create(WS_MSG_TYPE_ERROR, NULL, err_parsed) : NULL;
    if (error_msg) {
        ws_send_message(wsi, error_msg);
        ws_message_destroy(error_msg);
    }
    if (err_parsed)
        cJSON_Delete(err_parsed);
    AIRY_FREE(error_json);
}

static int ws_est(ws_gateway_t *gateway, struct lws *wsi,
                  ws_connection_context_t **context, void **user)
{
    if (!gateway->rate_limiter)
        return handle_ws_established(gateway, context, user);

    char client_ip[64];
    client_ip[0] = '\0';
    /* 用 lws_get_peer_simple() 而非 lws_get_peer_addresses()：后者解引用
     * wsi->a.vhost，在 libwebsockets 4.3.3 上段错误（上游 issue #2433）。 */
    lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
    if (client_ip[0] == '\0')
        snprintf(client_ip, sizeof(client_ip), "_unresolved");
    if (gateway_rate_limiter_allow(gateway->rate_limiter, client_ip))
        return handle_ws_established(gateway, context, user);

    AIRY_LOG_WARN("WebSocket rate limit exceeded for %s, closing connection", client_ip);
    ws_err_reply(wsi, -32004, "Rate limit exceeded");
    return -1;
}

static int ws_recv(ws_gateway_t *gateway, struct lws *wsi,
                   ws_connection_context_t *context, void *in, size_t len)
{
    /* P0.19（仅 Debug 崩溃）：GCC -O0 会把 CJSON_AUTO_FREE 的清理属性扩展到函数
     * 尾声，包括声明之前的提前返回路径；因此必须在任何 return 之前以 NULL 初始化
     * 声明，保证清理是无操作。Release 构建恰好把死清理优化掉，掩盖了该缺陷。 */
    CJSON_AUTO_FREE cJSON *json = NULL;

    if (!context) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "handle_ws_established: failed");
        return AIRY_ERR_UNKNOWN;
    }

    if (len > gateway->max_request_size) {
        ws_err_reply(wsi, -32603, "Message too large");
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "ws_send_message: IO error");
        return AIRY_ERR_UNKNOWN;
    }

    context->last_activity_ns = gateway_time_ns();
    context->messages_received++;
    context->bytes_received += len;
    atomic_fetch_add(&gateway->messages_total, 1);
    atomic_fetch_add(&gateway->bytes_received, len);

    /* P0：lws 的 in 缓冲区不以 '\0' 结尾，cJSON_Parse 会越界读，
     * 必须按 len 用 cJSON_ParseWithLength 解析。 */
    json = cJSON_ParseWithLength((const char *)in, len);
    if (!json) {
        ws_err_reply(wsi, -32700, "Parse error");
        return 0;
    }

    cJSON *type = cJSON_GetObjectItem(json, "type");
    if (!type || !cJSON_IsString(type))
        return handle_ws_unknown_message(wsi, "missing type field");

    const char *type_str = type->valuestring;
    if (strcmp(type_str, "ping") == 0)
        return handle_ws_ping(context, wsi);
    if (strcmp(type_str, "rpc_request") == 0) {
        cJSON *rpc_request = cJSON_GetObjectItem(json, "payload");
        return rpc_request ? handle_ws_rpc_request(gateway, context, rpc_request, wsi) : 0;
    }
    return handle_ws_unknown_message(wsi, type_str);
}

/**
  * @brief WebSocket callback function
  *
  * 单责分派：每个 reason 只做上下文解析与调用转发，具体处置在
  * ws_est() / ws_recv() / handle_ws_closed() 内完成。
  *
  * @param wsi WebSocket instance
  * @param reason Callback reason
  * @param user User pointer
  * @param in Input data
  * @param len Data length
  * @return 0 on success, -1 on failure
 */
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                       size_t len)
{
    ws_gateway_t *gateway = (ws_gateway_t *)lws_context_user(lws_get_context(wsi));
    ws_connection_context_t *context = NULL;

    /* 连接建立前的回调（如 lws_create_context 期间触发的 PROTOCOL_INIT/DESTROY）
     * user 为 NULL，解引用会段错误；仅对已建立连接的回调解析 per_session_data。 */
    if (user)
        context = (ws_connection_context_t *)*(void **)user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        return ws_est(gateway, wsi, &context, &user);

    case LWS_CALLBACK_RECEIVE:
        return ws_recv(gateway, wsi, context, in, len);

    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
    case LWS_CALLBACK_CLOSED_HTTP:
        return handle_ws_closed(gateway, &context, user);

    default:
        break;
    }

    return 0;
}

#endif /* GATEWAY_HAS_WS */
