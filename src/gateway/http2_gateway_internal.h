// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_internal.h
 * @brief Macros and cross-file declarations shared by the HTTP/2 gateway split files.
 */

#ifndef AIRY_RT_GATEWAY_HTTP2_INTERNAL_H
#define AIRY_RT_GATEWAY_HTTP2_INTERNAL_H

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

#include "atomic_compat.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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

#define HTTP2_RECV_BUF_SIZE (64 * 1024)
#define HTTP2_SEND_BUF_SIZE (64 * 1024)
#define HTTP2_MAX_HEADER_SIZE (64 * 1024)
#define HTTP2_DEFAULT_MAX_STREAMS 128
#define HTTP2_DEFAULT_TIMEOUT_SEC 60
#define HTTP2_POLL_TIMEOUT_MS 1000
#define HTTP2_LISTEN_BACKLOG 128 /**< listen backlog */
#define HTTP2_INITIAL_WINDOW_SIZE 65535

/**
  * @brief Internal handler adapter.
 *
 * Adapts gateway_internal_handler_t (internal signature) to the custom_handler
 * signature required by gateway_protocol_handle_request.
 */
typedef struct {
    gateway_internal_handler_t internal_handler;
    void *internal_data;
} http2_handler_adapter_t;

/* Helpers shared across files (was static; now external linkage) **/
http2_stream_context_t *http2_stream_create(int32_t stream_id);
void http2_stream_destroy(http2_stream_context_t *ctx);
int http2_stream_append_body(http2_stream_context_t *ctx, const uint8_t *data, size_t len);
int http2_stream_set_header_str(char **dst, const uint8_t *value, size_t vlen);
int http2_internal_handler_adapter(const char *request_json, char **response_json,
                                   void *user_data);
ssize_t http2_data_source_read_callback(nghttp2_session *session, int32_t stream_id,
                                        uint8_t *buf, size_t length, uint32_t *data_flags,
                                        nghttp2_data_source *source, void *user_data);
size_t http2_build_response_headers(http2_stream_context_t *ctx, http2_gateway_t *gw,
                                    nghttp2_nv *nva, size_t max_nva);
void http2_free_response_headers(nghttp2_nv *nva, size_t count);
int http2_submit_response_impl(nghttp2_session *session, http2_stream_context_t *ctx,
                               http2_gateway_t *gw);
char *http2_handle_jsonrpc(http2_gateway_t *gw, http2_stream_context_t *ctx);
char *http2_handle_health(void);
char *http2_handle_preflight(void);
void http2_process_request(nghttp2_session *session, int32_t stream_id, void *user_data);
int http2_on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
int http2_on_header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name,
                    size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags,
                    void *user_data);
int http2_on_data_chunk_recv(nghttp2_session *session, uint8_t flags, int32_t stream_id,
                             const uint8_t *data, size_t len, void *user_data);
int http2_on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
int http2_on_stream_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
                          void *user_data);
int http2_create_callbacks(nghttp2_session_callbacks **callbacks);
http2_gateway_session_t *http2_session_create(http2_gateway_t *gw, int fd);
void http2_session_destroy(http2_gateway_session_t *sess);
int http2_session_recv_data(http2_gateway_session_t *sess);
int http2_session_send_data(http2_gateway_session_t *sess);
int http2_gateway_add_session(http2_gateway_t *gw, http2_gateway_session_t *sess);
void http2_gateway_remove_session(http2_gateway_t *gw, size_t index);
void http2_event_loop_accept(http2_gateway_t *gw);
void http2_event_loop_cleanup(http2_gateway_t *gw);
void *http2_event_loop(void *arg);
int http2_set_nonblocking(int fd);
void http2_set_reuseaddr(int fd);

#endif /* AIRY_HAS_HTTP2 */

#endif /* AIRY_RT_GATEWAY_HTTP2_INTERNAL_H */
