// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file ws_gateway_message.c
 * @brief WebSocket gateway - message encoding and request handling.
 *
 * Implements the WebSocket message model (create/destroy/serialize/send)
 * and the per-reason request handlers (RPC dispatch, ping, close, unknown
 * message), single responsibility. Split out of ws_gateway.c.
 */

// @owner: team-B
#include "ws_gateway_internal.h"

#include "gateway_rpc_handler.h"
#include "gateway_utils.h"
#include "jsonrpc.h"

#include "airy_memory.h"
#include "error.h"

#ifdef GATEWAY_HAS_WS

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <string.h>

/**
  * @brief Create a WebSocket message
  * @param type Message type
  * @param session_id Session ID (may be NULL)
  * @param payload Message payload (may be NULL)
  * @return Message struct pointer, or NULL on failure
 */
ws_message_t *ws_message_create(ws_message_type_t type, const char *session_id, cJSON *payload)
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
void ws_message_destroy(ws_message_t *msg)
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
int ws_send_message(struct lws *wsi, ws_message_t *msg)
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
int handle_ws_established(ws_gateway_t *gateway, ws_connection_context_t **context_ptr,
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
int handle_ws_ping(ws_connection_context_t *context, struct lws *wsi)
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
int handle_ws_rpc_request(ws_gateway_t *gateway, ws_connection_context_t *context,
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
int handle_ws_unknown_message(struct lws *wsi, const char *unknown_type)
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
int handle_ws_closed(ws_gateway_t *gateway, ws_connection_context_t **context_ptr,
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

#endif /* GATEWAY_HAS_WS */
