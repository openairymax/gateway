// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file ws_gateway.c
 * @brief WebSocket gateway implementation - libwebsockets integration.
 *
 * Implements the WebSocket bidirectional communication protocol and talks
 * to the kernel through the syscall interface. The gateway only translates
 * protocols and contains no business logic.
 *
 * Design principles:
 *   K-1 minimal core: only protocol translation, zero business logic
 *   S-2 layered decomposition: single responsibility per layer
 *   E-8 testability: route handlers independently testable
 */

// @owner: team-B
#include "ws_gateway.h"

#include "../utils/gateway_rate_limiter.h"
#include "../utils/gateway_rpc_handler.h"
#include "../utils/gateway_utils.h"
#include "../utils/jsonrpc.h"
#include "../utils/syscall_router.h"

#include "logging.h"

#ifdef GATEWAY_HAS_WS

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atomic_compat.h"

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

struct ws_gateway;
typedef struct ws_gateway ws_gateway_t;

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

/**
  * @brief WebSocket connection context
 */
typedef struct ws_connection_context {
    struct lws *wsi;
    char *session_id;
    char *remote_addr;
    uint64_t connect_time_ns;
    uint64_t last_activity_ns;

    size_t messages_sent;
    size_t messages_received;
    size_t bytes_sent;
    size_t bytes_received;
} ws_connection_context_t;

/**
  * @brief WebSocket gateway internal structure
 */
struct ws_gateway {
    struct lws_context *context;
    uint16_t port;
    char *host;

    gateway_rate_limiter_t *rate_limiter;

    void *handler_adapter;
    gateway_internal_handler_t handler;
    void *handler_data;

    atomic_bool running;

    atomic_uint_fast64_t connections_total;
    atomic_uint_fast64_t connections_active;
    atomic_uint_fast64_t messages_total;
    atomic_uint_fast64_t bytes_sent;
    atomic_uint_fast64_t bytes_received;

    size_t max_request_size;
    void *event_thread;
};

/**
  * @brief WebSocket message type
 */
typedef enum {
    WS_MSG_TYPE_PING = 1,
    WS_MSG_TYPE_PONG,
    WS_MSG_TYPE_RPC_REQUEST,
    WS_MSG_TYPE_RPC_RESPONSE,
    WS_MSG_TYPE_NOTIFICATION,
    WS_MSG_TYPE_ERROR
} ws_message_type_t;

/**
  * @brief WebSocket message structure
 */
typedef struct ws_message {
    ws_message_type_t type;
    char *session_id;
    cJSON *payload;
    uint64_t timestamp_ns;
} ws_message_t;

/**
  * @brief Create a WebSocket message
  * @param type Message type
  * @param session_id Session ID (may be NULL)
  * @param payload Message payload (may be NULL)
  * @return Message struct pointer, or NULL on failure
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
  * @brief Destroy a WebSocket message
  * @param msg Message struct pointer
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
  * @brief Serialize a WebSocket message to a JSON string
  * @param msg Message struct pointer
  * @return JSON string; caller must AIRY_FREE()
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
  * @brief Send a WebSocket message
  * @param wsi WebSocket instance
  * @param msg Message struct pointer
  * @return Bytes sent on success, -1 on failure
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

    /* P0: lws_write() requires LWS_SEND_BUFFER_PRE_PADDING bytes of valid
     * writable storage BEFORE the payload and LWS_SEND_BUFFER_POST_PADDING
     * bytes AFTER it, so the protocol header/trailer can be written in-situ.
     * cJSON_PrintUnformatted() returns a bare malloc() buffer; passing it
     * directly lets lws overwrite memory before the pointer (heap underrun),
     * which crashes under Release/LTO while Debug happens to survive.
     * Copy the payload into a padded buffer instead. */
    unsigned char *send_buf = AIRY_MALLOC(LWS_SEND_BUFFER_PRE_PADDING + out_len +
                                          LWS_SEND_BUFFER_POST_PADDING);
    if (!send_buf) {
        AIRY_FREE(json_str);
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "ws_send_message: allocation failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    memcpy(send_buf + LWS_SEND_BUFFER_PRE_PADDING, json_str, out_len);

    int result = lws_write(wsi, send_buf + LWS_SEND_BUFFER_PRE_PADDING, out_len, LWS_WRITE_TEXT);

    AIRY_FREE(send_buf);
    AIRY_FREE(json_str);
    return result;
}

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
  * @brief Handle an RPC request (via the unified RPC handler)
 *
  * Unified request handling via gateway_rpc_handle_request(),
  * removing duplication with the HTTP/stdio gateways.
 *
 * @param gateway Gateway instance
  * @param request JSON-RPC request object
  * @return JSON response string; caller must AIRY_FREE()
 */
static char *handle_rpc_request(ws_gateway_t *gateway, cJSON *request)
{
    if (!gateway || !request) {
        return jsonrpc_create_error_response(NULL, -32600, "Invalid request", NULL);
    }

    rpc_result_t result = gateway_rpc_handle_request(request, ws_rpc_handler_adapter, gateway);

    if (result.error_code != 0 || !result.response_json) {
        char *error_resp = result.response_json ?
                               result.response_json :
                               jsonrpc_create_error_response(NULL, -32603, "Internal error", NULL);
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

/**
  * @brief Handle connection establishment
 * @param gateway Gateway instance
  * @param context Connection context
  * @param user User pointer
  * @return 0 on success, -1 on failure
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
  * @brief Handle Ping messages
  * @param context Connection context
  * @param wsi WebSocket instance
  * @return 0 on success
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
  * @brief Handle RPC request messages
 * @param gateway Gateway instance
  * @param context Connection context
 * @param rpc_request RPC request object
  * @param wsi WebSocket instance
  * @return 0 on success
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

    CJSON_PARSE_GUARD(response_json, response, {
        AIRY_FREE(response);
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "cJSON_Parse: parse error");
        return AIRY_ERR_UNKNOWN;
    });

    ws_message_t *response_msg =
        ws_message_create(WS_MSG_TYPE_RPC_RESPONSE, context->session_id, response_json);

    if (response_msg) {
        ws_send_message(wsi, response_msg);
        ws_message_destroy(response_msg);
    }

    AIRY_FREE(response);
    return 0;
}

/**
  * @brief Handle unknown message types
  * @param wsi WebSocket instance
  * @param unknown_type Unknown type string
  * @return 0 on success
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
  * @brief Handle connection close
 * @param gateway Gateway instance
  * @param context_ptr Connection context pointer
  * @param user lws per-session data slot (cleared to NULL on close)
  * @return 0 on success
 *
 * P0: libwebsockets may fire two close callbacks for a single connection
 * (e.g. LWS_CALLBACK_WS_PEER_INITIATED_CLOSE followed by LWS_CALLBACK_CLOSED).
 * Without clearing the per-session data slot, the second callback dereferences
 * a dangling pointer and double-frees the context.
 */
static int handle_ws_closed(ws_gateway_t *gateway, ws_connection_context_t **context_ptr,
                            void *user)
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
    if (user)
        *(void **)user = NULL;

    return 0;
}

/**
  * @brief WebSocket callback function
 *
  * Uses the routing pattern, splitting handling of each reason into its own function,
  * greatly reducing cyclomatic complexity and improving testability.
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

    /* P0.19 (Debug-only crash): the RPC request JSON is parsed only inside the
      * LWS_CALLBACK_RECEIVE branch below, but GCC -O0 widens the CJSON_AUTO_FREE
      * cleanup attribute to the function epilogue, so the cleanup handler also
      * runs on paths that never enter that branch (e.g. PROTOCOL_INIT / WSI_CREATE
      * fired during lws_create_context). Declaring the pointer at function scope
      * with an unconditional NULL initializer guarantees the epilogue cleanup is
      * a no-op whenever the variable was never assigned; Release builds happened
      * to eliminate the dead cleanup call, which masked the bug. */
    CJSON_AUTO_FREE cJSON *json = NULL;

    /* Callbacks before connection setup (e.g. LWS_CALLBACK_PROTOCOL_INIT/DESTROY,
      * fired during lws_create_context) have user == NULL; dereferencing would segfault;
      * resolve per_session_data only for established-connection callbacks. */
    if (user)
        context = (ws_connection_context_t *)*(void **)user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        if (gateway->rate_limiter) {
            char client_ip[64];
            client_ip[0] = '\0';
            /* Use lws_get_peer_simple() instead of lws_get_peer_addresses():
             * the latter dereferences wsi->a.vhost and segfaults on
             * libwebsockets 4.3.3 (upstream issue #2433). */
            lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
            if (client_ip[0] == '\0') {
                snprintf(client_ip, sizeof(client_ip), "_unresolved");
            }
            if (!gateway_rate_limiter_allow(gateway->rate_limiter, client_ip)) {
                AIRY_LOG_WARN("WebSocket rate limit exceeded for %s, closing connection",
                         client_ip);
                char *error_json =
                    jsonrpc_create_error_response(NULL, -32004, "Rate limit exceeded", NULL);
                if (error_json) {
                    /* cJSON_Parse result is deep-copied by ws_message_create;
                     * release both the parsed tree and the JSON string. */
                    cJSON *err_parsed = cJSON_Parse(error_json);
                    ws_message_t *error_msg = err_parsed ?
                        ws_message_create(WS_MSG_TYPE_ERROR, NULL, err_parsed) : NULL;
                    if (error_msg) {
                        ws_send_message(wsi, error_msg);
                        ws_message_destroy(error_msg);
                    }
                    if (err_parsed)
                        cJSON_Delete(err_parsed);
                    AIRY_FREE(error_json);
                }
                return -1;
            }
        }
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

        context->last_activity_ns = gateway_time_ns();
        context->messages_received++;
        context->bytes_received += len;
        atomic_fetch_add(&gateway->messages_total, 1);
        atomic_fetch_add(&gateway->bytes_received, len);

        /* P0: lws's in buffer is not '\0'-terminated; cJSON_Parse would overrun;
          * parse by len with cJSON_ParseWithLength (json is a function-scope
          * CJSON_AUTO_FREE pointer, declared NULL-initialized above). */
        json = cJSON_ParseWithLength((const char *)in, len);
        if (!json) {

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

        cJSON *type = cJSON_GetObjectItem(json, "type");
        if (!type || !cJSON_IsString(type)) {

            return handle_ws_unknown_message(wsi, "missing type field");
        }

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

        return result;

    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
    case LWS_CALLBACK_CLOSED_HTTP:
        return handle_ws_closed(gateway, &context, user);

    default:
        break;
    }

    return 0;
}

#ifndef _WIN32
/**
  * @brief libwebsockets event-loop thread
 *
  * lws_create_context only creates the context; lws_service must be called
  * to drive I/O (IRON-2: a real WebSocket server, not a stub).
  * 50ms timeout lets the thread exit and join promptly after running=false.
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
        /* lws_cancel_service safely wakes the lws_service loop from other threads,
          * so the event thread exits right after running=false. Otherwise the loop
          * waits for the next 50ms poll timeout; under netlink event storms,
          * lws_service may process events for long, blocking join for seconds,
          * and the graceful exit exceeds the external stop threshold, getting KILLed. */
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

    if (gateway->rate_limiter) {
        gateway_rate_limiter_destroy(gateway->rate_limiter);
        gateway->rate_limiter = NULL;
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

static airy_err_t ws_gateway_set_handler(void *gateway_impl, gateway_internal_handler_t handler,
                                         void *user_data)
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

/**
  * @brief Create a WebSocket gateway instance
  * @param host Listen address (e.g. "127.0.0.1", "0.0.0.0"); must not be NULL
  * @param port Listen port (e.g. 8081)
  * @return Gateway handle, or NULL on failure (OOM or invalid args)
 *
 * @ownership Caller must release via gateway_destroy()
 * @threadsafe yes
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

    /* Rate limiting is opt-in, driven by the same env vars as the HTTP/2
     * gateway (GATEWAY_RATE_LIMIT_ENABLED=true [+ GATEWAY_RATE_LIMIT_RPS]). */
    gateway->rate_limiter = NULL;
    const char *rate_limit_enabled = getenv("GATEWAY_RATE_LIMIT_ENABLED");
    if (rate_limit_enabled && strcmp(rate_limit_enabled, "true") == 0) {
        gateway_rate_limit_config_t rl_config;
        gateway_rate_limiter_get_default_config(&rl_config);
        rl_config.enabled = true;
        const char *rps = getenv("GATEWAY_RATE_LIMIT_RPS");
        if (rps) {
            long v = strtol(rps, NULL, 10);
            if (v > 0 && v <= 100000) {
                rl_config.requests_per_second = (uint32_t)v;
            } else {
                AIRY_LOG_WARN("ignoring invalid GATEWAY_RATE_LIMIT_RPS: %s", rps);
            }
        }
        gateway->rate_limiter = gateway_rate_limiter_create(&rl_config);
        AIRY_LOG_INFO("WebSocket rate limiting enabled (rps=%u)",
                 rl_config.requests_per_second);
    }

    if (!gateway->host) {
        if (gateway->rate_limiter) {
            gateway_rate_limiter_destroy(gateway->rate_limiter);
        }
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
        if (gateway->rate_limiter) {
            gateway_rate_limiter_destroy(gateway->rate_limiter);
        }
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
