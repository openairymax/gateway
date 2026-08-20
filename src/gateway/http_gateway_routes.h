/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file http_gateway_routes.h
 * @brief HTTP gateway route table declarations.
 *
 * Uses a route-table pattern to reduce the cyclomatic complexity of
 * handle_http_request, splitting each route handler into its own function.
 *
 * Design principles:
 *   E-8 testability: each route handler can be tested independently
 *   K-1 minimal core: route functions only dispatch, no business logic
 */

/* @owner: team-B */
#ifndef HTTP_GATEWAY_ROUTES_H
#define HTTP_GATEWAY_ROUTES_H

#include <stdlib.h>
#include <string.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#else
struct cJSON;
typedef struct cJSON cJSON;
#endif


#include "atomic_compat.h"
#include "http_gateway.h"
#include "jsonrpc.h"
#include "syscall_router.h"
#include "syscalls.h"


/**
  * @brief Handle JSON-RPC POST requests
 */
int handle_post_jsonrpc(http_gateway_t *gateway, struct MHD_Connection *connection,
                        http_request_context_t *context);

/**
  * @brief Handle OPTIONS preflight requests (CORS)
 */
int handle_options_preflight(http_gateway_t *gateway, struct MHD_Connection *connection,
                             http_request_context_t *context);

/**
  * @brief Handle GET /health health checks
 */
int handle_health_check(http_gateway_t *gateway, struct MHD_Connection *connection,
                        http_request_context_t *context);

/**
  * @brief Handle GET /metrics
 */
int handle_metrics_export(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context);

/**
  * @brief Handle POST /api/v1/chat/stream (SSE streaming chat)
  *
  * The gateway proxies llm_d complete_stream: parses OpenAI messages /
  * simplified JSON-RPC agent.run bodies, pulls chunks from llm_d and forwards
  * them as SSE events (data: <chunk>\n\n, then data: [DONE] at EOF).
 */
int handle_chat_stream_sse(http_gateway_t *gateway, struct MHD_Connection *connection,
                           http_request_context_t *context);

/**
  * @brief Handle GET /api/v1/hall/watch (SSE hall event push)
  *
  * Long-lived SSE subscription over the hall event flow: every newly
  * recorded hall event from any writer process is pushed in global
  * (ts_utc, seq) order. Real-time push counterpart of hall.stream (pull).
 */
int handle_hall_watch_sse(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context);

/**
  * @brief Handle 404 Not Found
 */
int handle_not_found(http_gateway_t *gateway, struct MHD_Connection *connection,
                     http_request_context_t *context);

/**
  * @brief Handle request-too-large errors
 */
int handle_request_too_large(http_gateway_t *gateway, struct MHD_Connection *connection,
                             http_request_context_t *context, size_t data_size);

/**
  * @brief Handle JSON parse errors
 */
int handle_parse_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                       http_request_context_t *context, size_t data_size);


/**
  * @brief HTTP route entry
 */
typedef struct {
    const char *method;
    const char *path;
    int (*handler)(http_gateway_t *, struct MHD_Connection *, http_request_context_t *);
} http_route_t;

typedef int (*http_route_handler_t)(http_gateway_t *, struct MHD_Connection *,
                                    http_request_context_t *);


/**
  * @brief HTTP request entry point
 *
  * Cyclomatic complexity reduced from ~25 to ~8 via the route-table pattern.
 *
  * @param cls Gateway instance pointer (http_gateway_t*)
 * @param connection MHD connection object
 * @param url Request URL
 * @param method HTTP method
 * @param version HTTP version
  * @param upload_data Upload data
  * @param upload_data_size Data size
  * @param con_cls Connection context
 * @return MHD_YES/MHD_NO
 */
int handle_http_request(void *cls, struct MHD_Connection *connection, const char *url,
                        const char *method, const char *version, const char *upload_data,
                        size_t *upload_data_size, void **con_cls);

#endif /* HTTP_GATEWAY_ROUTES_H */
