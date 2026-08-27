/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file ws_gateway_internal.h
 * @brief WebSocket gateway internal shared definitions.
 *
 * After ws_gateway.c was split by functional domain, this header carries
 * the shared object layouts and cross-file function declarations:
 *   - ws_gateway.c           gateway lifecycle / ops / creation
 *   - ws_gateway_message.c   message encode + request handling
 *   - ws_gateway_callback.c  libwebsockets event callback dispatch
 */

#ifndef AIRY_RT_GATEWAY_WS_INTERNAL_H
#define AIRY_RT_GATEWAY_WS_INTERNAL_H

#include "gateway_internal.h"
#include "gateway_rate_limiter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atomic_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque libwebsockets types (forward declarations; the full header is
 * only pulled in under GATEWAY_HAS_WS). */
struct lws;
struct lws_context;
struct cJSON;

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

typedef struct ws_gateway ws_gateway_t;

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
    struct cJSON *payload;
    uint64_t timestamp_ns;
} ws_message_t;

/* Message + request-handling domain (ws_gateway_message.c) */
ws_message_t *ws_message_create(ws_message_type_t type, const char *session_id, struct cJSON *payload);
void ws_message_destroy(ws_message_t *msg);
int ws_send_message(struct lws *wsi, ws_message_t *msg);
int handle_ws_established(ws_gateway_t *gateway, ws_connection_context_t **context_ptr,
                          void **user);
int handle_ws_ping(ws_connection_context_t *context, struct lws *wsi);
int handle_ws_rpc_request(ws_gateway_t *gateway, ws_connection_context_t *context,
                          struct cJSON *rpc_request, struct lws *wsi);
int handle_ws_unknown_message(struct lws *wsi, const char *unknown_type);
int handle_ws_closed(ws_gateway_t *gateway, ws_connection_context_t **context_ptr, void *user);

#ifdef GATEWAY_HAS_WS
#include <libwebsockets.h>

/* Event callback dispatch (ws_gateway_callback.c) */
extern const struct lws_protocols ws_protocols[];
#endif /* GATEWAY_HAS_WS */

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_WS_INTERNAL_H */
