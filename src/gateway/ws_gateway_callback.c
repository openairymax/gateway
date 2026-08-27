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
#include "jsonrpc.h"

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

#endif /* GATEWAY_HAS_WS */
