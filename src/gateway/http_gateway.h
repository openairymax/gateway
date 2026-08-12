/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file http_gateway.h
 * @brief HTTP gateway interface.
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
  * @brief CORS configuration structure
 *
  * Security settings for Cross-Origin Resource Sharing (CORS)。
  * Configure a whitelist in production; allow all origins in development。
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
  * @brief Create an HTTP gateway
 *
 * @param host Listen address
 * @param port Listen port
 * @return Gateway instance, or NULL on failure
 *
 * @ownership Caller must release via gateway_destroy()
 */
gateway_t *http_gateway_create(const char *host, uint16_t port);

/**
  * @brief Handle a JSON-RPC request
 */
char *handle_jsonrpc_request(http_gateway_t *gateway, http_request_context_t *context);

/**
  * @brief Create an HTTP response
 */
struct MHD_Response *create_http_response(int status_code, const char *content, size_t content_len);

/**
  * @brief Create an HTTP response (CORS-safe variant)
 *
 * Automatically set CORS headers per gateway config; prefer this in all route handlers.
 *
 * @param gateway HTTP gateway instance (for CORS config)
 * @param connection MHD connection object (for the Origin header)
 * @param status_code HTTP status code (currently unused; kept for future extension)
  * @param content Response content
  * @param content_len Content length
  * @return MHD response object
 */
struct MHD_Response *create_http_response_ex(http_gateway_t *gateway,
                                             struct MHD_Connection *connection, int status_code,
                                             const char *content, size_t content_len);

/**
  * @brief HTTP request handler callback type
 */
typedef int (*http_request_handler_t)(void *cls, struct MHD_Connection *connection, const char *url,
                                      const char *method, const char *version,
                                      const char *upload_data, size_t *upload_data_size,
                                      void **con_cls);

/**
  * @brief HTTP request handler function
 */
int handle_http_request(void *cls, struct MHD_Connection *connection, const char *url,
                        const char *method, const char *version, const char *upload_data,
                        size_t *upload_data_size, void **con_cls);

/**
  * @brief Apply secure HTTP response headers
 *
 * Adds security-related HTTP headers such as X-Content-Type-Options,
 * X-Frame-Options, etc.
 *
  * @param response MHD response object
 */
void gateway_apply_security_headers(struct MHD_Response *response);

/**
  * @brief Apply CORS response headers
 *
 * Sets CORS response headers automatically based on the gateway CORS config
 * and the request Origin header. Call it everywhere MHD_Response objects are
 * created directly (alongside gateway_apply_security_headers).
 *
  * @param gateway HTTP gateway instance
  * @param connection MHD connection object
  * @param response MHD response object
 */
void gateway_apply_cors_headers(http_gateway_t *gateway, struct MHD_Connection *connection,
                                struct MHD_Response *response);

/**
  * @brief Parse a JSON request body
 *
  * @param gateway HTTP gateway instance
  * @param context Request context
  * @param data Request body data
  * @param size Data size
 * @return 0 on success, non-zero on failure
 */
int parse_json_request(http_gateway_t *gateway, http_request_context_t *context, const char *data,
                       size_t size);

/**
  * @brief Register a dynamic endpoint on the HTTP gateway
 *
  * @param gateway HTTP gateway instance
  * @param method HTTP method (copied internally)
  * @param path URL path (copied internally)
  * @param handler Endpoint handler callback
  * @param user_data User data passed to the callback
  * @return 0 on success, -1 invalid args, -2 out of memory
 */
int http_gateway_register_endpoint(http_gateway_t *gateway, const char *method, const char *path,
                                   gateway_endpoint_handler_t handler, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_HTTP_H */
