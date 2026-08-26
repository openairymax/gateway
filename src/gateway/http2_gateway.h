/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file http2_gateway.h
 * @brief HTTP/2 gateway interface - nghttp2-based HTTP/2 server.
 *
 * Extends http_gateway_t with HTTP/2 protocol support, reusing the JSON-RPC
 * routing logic and the gateway_protocol_handler multi-protocol processor.
 *
 * Design principles:
 *   IRON-2: no stubs or simplified features; a truly usable HTTP/2 server
 *   K-1 minimal core: only protocol translation, zero business logic
 *   S-2 layered decomposition: single responsibility per layer
 */

/* @owner: team-B */
#ifndef AIRY_RT_GATEWAY_HTTP2_H
#define AIRY_RT_GATEWAY_HTTP2_H

#include "gateway_internal.h"
#include "http_gateway.h"

#include <nghttp2/nghttp2.h>

#include "atomic_compat.h"

#ifndef _WIN32
#include <netinet/in.h>
#else
#include <ws2tcpip.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP/2 stream context.
 *
 * Each HTTP/2 stream maps to a request context storing request headers,
 * request body and response data.
 */
typedef struct http2_stream_context {
    int32_t stream_id;
    char *method;
    char *path;
    char *content_type;
    char *origin;

    uint8_t *request_body;
    size_t request_body_len;
    size_t request_body_cap;
    char *response_body;
    size_t response_body_len;
    size_t response_sent;
    int response_status;
    bool headers_complete;
    bool body_complete;
    bool response_sent_flag;

    /* Active-stream list linkage (owned by http2_gateway_session_t).
     * nghttp2 does NOT invoke on_stream_close for streams still alive when
     * the session is destroyed, so the session keeps its own list to free
     * any leaked stream contexts on teardown. */
    struct http2_stream_context *next_active;
} http2_stream_context_t;

/**
 * @brief HTTP/2 session (per connection).
 *
 * Each accepted TCP connection maps to one nghttp2 server session.
 */
typedef struct http2_gateway_session {
    int fd; /**< TCP socket fd */
    nghttp2_session *session;
    struct http2_gateway *gateway;
    char client_ip[INET6_ADDRSTRLEN]; /**< peer IP string (rate limiting / audit) */
    uint64_t connect_time_ns;
    uint64_t last_activity_ns;
    bool closing;

    /* Active streams on this session; freed on session teardown (P1: streams
     * still open when the connection dies would otherwise leak). */
    http2_stream_context_t *active_streams;

    /* P0 fix: partial-write buffer. nghttp2_session_mem_send() considers the
     * data consumed once returned, but write() may write only part of it;
     * the remainder must be buffered until the next POLLOUT. */
    uint8_t *pending_send_buf;
    size_t pending_send_len;
    size_t pending_send_offset;
} http2_gateway_session_t;

/**
 * @brief HTTP/2 gateway extension structure.
 *
 * Adds HTTP/2 session management on top of http_gateway_t. The base field
 * is the first member to allow safe casts to http_gateway_t*.
 */
typedef struct http2_gateway {
    http_gateway_t base;
    int listen_fd;
    http2_gateway_session_t **sessions;
    size_t session_count;
    size_t session_capacity;
    atomic_bool running;
    void *event_thread;
    unsigned int max_concurrent_streams;
    unsigned int connection_timeout;
} http2_gateway_t;

/**
  * @brief Create HTTP/2 gateway
 *
 * Creates an HTTP/2 gateway instance, reusing the HTTP/1.1 routing logic and
 * protocol handlers. Start it with gateway_start() and destroy with gateway_destroy().
 *
 * @param host Listen address (e.g. "127.0.0.1", "0.0.0.0")
 * @param port Listen port
 * @return Gateway instance, or NULL on failure
 *
 * @ownership Caller must release via gateway_destroy()
 */
gateway_t *http2_gateway_create(const char *host, uint16_t port);

/**
  * @brief Start HTTP/2 gateway
 *
 * Creates the listening socket and starts the event-loop thread.
 *
 * @param gw HTTP/2 gateway instance
 * @return AIRY_SUCCESS on success, negative error code on failure
 */
int http2_gateway_start(http2_gateway_t *gw);

/**
  * @brief Stop HTTP/2 gateway
 *
 * Sets the running flag to false, closes the listening socket and waits for
 * the event-loop thread to exit.
 *
 * @param gw HTTP/2 gateway instance
 * @return AIRY_SUCCESS on success
 */
int http2_gateway_stop(http2_gateway_t *gw);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_HTTP2_H */
