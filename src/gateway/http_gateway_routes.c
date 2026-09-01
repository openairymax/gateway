// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file http_gateway_routes.c
 * @brief HTTP gateway route handler implementations.
 *
 * Splits the complex handle_http_request logic into separate route handler
 * functions to lower cyclomatic complexity and improve maintainability.
 */

// @owner: team-B
#include "http_gateway_routes.h"

#include "gateway_rate_limiter.h"
#include "gateway_rpc_handler.h"
#include "gateway_utils.h"
#include "http_gateway.h"
#include "jsonrpc.h"
#include "logging.h"
#include "airy_memory.h"
#include "platform.h"
#include "syscall_router.h"
#include "syscall_router_internal.h"
#include "syscalls.h"

#include <microhttpd.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif
#include <stdlib.h>
#include <string.h>

/* OpenAI tools schema shared with gateway_d (SSoT, one-to-one with tool_d) */
#include "airy_tool_schema.h"

/* Gateway-side hall event recording (write side of the SSoT event flow) */
#include "gateway_hall_store.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h> /* close() */
#endif

#include "atomic_compat.h"

/**
  * @brief Handle JSON-RPC POST requests (CC=3)
 */
int handle_post_jsonrpc(http_gateway_t *gateway, struct MHD_Connection *connection,
                        http_request_context_t *context)
{

    char *json_response = handle_jsonrpc_request(gateway, context);
    if (!json_response) {
        const char *err_msg = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":"
                              "\"Internal error\"},\"id\":null}";
        struct MHD_Response *response =
            create_http_response_ex(gateway, connection, 500, err_msg, strlen(err_msg));
        int ret = MHD_queue_response(connection, 500, response);
        MHD_destroy_response(response);
        return ret;
    }
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 200, json_response, strlen(json_response));

    atomic_fetch_add(&gateway->requests_total, 1);
    atomic_fetch_add(&gateway->bytes_received, context->body_len);
    atomic_fetch_add(&gateway->bytes_sent, strlen(json_response));

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    AIRY_FREE(json_response);
    return ret;
}

/**
  * @brief Handle OPTIONS requests (CORS preflight) (CC=2)
 */
int handle_options_preflight(http_gateway_t *gateway, struct MHD_Connection *connection,
                             http_request_context_t *context __attribute__((unused)))
{

    struct MHD_Response *response =
        MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);

    gateway_apply_security_headers(response);
    gateway_apply_cors_headers(gateway, connection, response);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);

    return ret;
}

/**
  * @brief Validate the API key (protects sensitive endpoints)
  * @param connection MHD connection object
 * @param gateway Gateway instance
  * @return true if the key is valid, false otherwise
 */
static bool gateway_verify_api_key(struct MHD_Connection *connection,
                                   http_gateway_t *gateway __attribute__((unused)))
{

    const char *env_key = getenv("GATEWAY_API_KEY");
    if (!env_key || !env_key[0])
        return false;

    const char *auth_header =
        MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
    if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0) {
        if (strcmp(auth_header + 7, env_key) == 0)
            return true;
    }

    const char *key_param =
        MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "api_key");
    if (key_param && strcmp(key_param, env_key) == 0)
        return true;

    return false;
}

/**
  * @brief URL path sanitization
  * @param url Raw URL path
  * @return true if safe, false if suspicious patterns detected
 */
static bool gateway_is_url_safe(const char *url)
{
    if (!url || !url[0])
        return false;

    size_t len = strlen(url);
    if (len > 2048)
        return false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c < 0x20 || c > 0x7E)
            return false;
    }

    if (strstr(url, "..") != NULL)
        return false;
    if (strstr(url, "%2e") != NULL || strstr(url, "%2E") != NULL)
        return false;
    if (strstr(url, "%3b") != NULL || strstr(url, "%3B") != NULL)
        return false;
    if (strstr(url, "%00") != NULL)
        return false;

    return true;
}

/**
  * @brief Handle GET /health (CC=2)
 */
int handle_health_check(http_gateway_t *gateway, struct MHD_Connection *connection,
                        http_request_context_t *context __attribute__((unused)))
{

    const char *health_json =
        "{\"status\":\"healthy\",\"service\":\"gateway\",\"version\":\"" GATEWAY_VERSION "\"}";
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 200, health_json, strlen(health_json));

    atomic_fetch_add(&gateway->requests_total, 1);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);

    return ret;
}

/**
  * @brief Handle GET /metrics (CC=3) - requires API key authentication
 */
int handle_metrics_export(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context __attribute__((unused)))
{

    if (!gateway_verify_api_key(connection, gateway)) {
        const char *err_json =
            "{\"error\":{\"code\":-32001,\"message\":\"Unauthorized: API key required\"}}";
        struct MHD_Response *response =
            create_http_response_ex(gateway, connection, 401, err_json, strlen(err_json));
        int ret = MHD_queue_response(connection, 401, response);
        MHD_destroy_response(response);
        atomic_fetch_add(&gateway->requests_failed, 1);
        return ret;
    }

    char *metrics_json = NULL;
    airy_err_t err = airy_sys_telemetry_metrics(&metrics_json);

    if (err != AIRY_SUCCESS || !metrics_json) {
        metrics_json = AIRY_STRDUP("{\"error\":\"failed to get metrics\"}");
    }

    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 200, metrics_json, strlen(metrics_json));
    AIRY_FREE(metrics_json);

    atomic_fetch_add(&gateway->requests_total, 1);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);

    return ret;
}

/**
  * @brief Handle 404 Not Found (CC=2)
 */
int handle_not_found(http_gateway_t *gateway, struct MHD_Connection *connection,
                     http_request_context_t *context)
{

    char *error_response = jsonrpc_create_error_response(NULL, -32601, "Not Found", NULL);
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 404, error_response, strlen(error_response));
    AIRY_FREE(error_response);

    atomic_fetch_add(&gateway->requests_failed, 1);

    int ret = MHD_queue_response(connection, 404, response);
    MHD_destroy_response(response);

    return ret;
}

/**
  * @brief Handle request-size-limit errors (CC=2)
 */
int handle_request_too_large(http_gateway_t *gateway, struct MHD_Connection *connection,
                             http_request_context_t *context __attribute__((unused)),
                             size_t data_size)
{

    char *error_response = jsonrpc_create_error_response(NULL, -413, "Request too large", NULL);
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 413, error_response, strlen(error_response));
    AIRY_FREE(error_response);

    atomic_fetch_add(&gateway->requests_failed, 1);
    atomic_fetch_add(&gateway->bytes_received, data_size);

    int ret = MHD_queue_response(connection, 413, response);
    MHD_destroy_response(response);

    return ret;
}

/**
  * @brief Handle JSON parse errors (CC=2)
 */
int handle_parse_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                       http_request_context_t *context __attribute__((unused)), size_t data_size)
{

    char *error_response = jsonrpc_create_error_response(NULL, -32700, "Parse error", NULL);
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 400, error_response, strlen(error_response));
    AIRY_FREE(error_response);

    atomic_fetch_add(&gateway->requests_failed, 1);
    atomic_fetch_add(&gateway->bytes_received, data_size);

    int ret = MHD_queue_response(connection, 400, response);
    MHD_destroy_response(response);

    return ret;
}

/**
  * @brief HTTP route table (priority-ordered)
  *
  * Route matching rules:
  * 1. 1. Match the HTTP method
  * 2. 2. Match the path ("*" wildcard supported)
  * 3. 3. Fall back to the default route (handle_not_found)
 */
static const http_route_t http_routes[] = {{"POST", "/", handle_post_jsonrpc, 0},
                                           {"POST", GW_SSE_CHAT_PATH, handle_chat_stream_sse, 1},
                                           {"POST", GW_SSE_RUN_STREAM_PATH, handle_run_stream_sse, 1},
                                           {"GET", "/api/v1/hall/watch", handle_hall_watch_sse, 0},
                                           {"OPTIONS", "*", handle_options_preflight, 0},
                                           {"GET", "/health", handle_health_check, 0},
                                           {"GET", "/metrics", handle_metrics_export, 0},
                                           {NULL, NULL, handle_not_found, 0}};

/**
  * @brief Whether the URL matches a streaming (SSE long-lived) route
  *
  * Streaming endpoints respond with a continuous event stream instead of a
  * one-shot JSON-RPC reply, so they must bypass the aggregation dispatch.
  * The streaming set is expressed by the route table itself (SSoT), not by
  * hard-coded paths in the dispatcher.
 */
static int is_streaming_route(const char *method, const char *url)
{
    for (const http_route_t *route = http_routes; route->method != NULL; route++) {
        if (route->streaming && strcmp(method, route->method) == 0 &&
            (strcmp(route->path, "*") == 0 || strcmp(url, route->path) == 0)) {
            return 1;
        }
    }
    return 0;
}

/**
  * @brief Find the matching route handler (CC=2)
 *
 * @param method HTTP method (e.g. "POST", "GET")
 * @param path URL path (e.g. "/", "/health")
  * @return Matching route handler, or NULL if none
 */
static http_route_handler_t find_http_route(const char *method, const char *path)
{
    for (const http_route_t *route = http_routes; route->method != NULL; route++) {
        if (strcmp(method, route->method) == 0) {
            if (strcmp(route->path, "*") == 0 || strcmp(path, route->path) == 0) {
                return route->handler;
            }
        }
    }
    return NULL;
}

/**
  * @brief Search and handle dynamically registered endpoints (CC=4)
 *
  * Bridge MHD request/response to gateway_endpoint_request_t / gateway_endpoint_response_t,
  * call the user handler, then bridge the response back to MHD.
 *
  * @param gateway HTTP gateway instance
  * @param connection MHD connection object
  * @param context Request context
 * @param method HTTP method
 * @param url Request URL
 * @return MHD_YES/MHD_NO
 */
static int handle_dynamic_endpoint_route(http_gateway_t *gateway, struct MHD_Connection *connection,
                                         http_request_context_t *context, const char *method,
                                         const char *url)
{
    const http_dynamic_endpoint_t *matched = NULL;

    for (size_t i = 0; i < gateway->dynamic_endpoint_count; i++) {
        const http_dynamic_endpoint_t *ep = &gateway->dynamic_endpoints[i];
        if (strcmp(method, ep->method) == 0 && strcmp(url, ep->path) == 0) {
            matched = ep;
            break;
        }
    }

    if (!matched) {
        return MHD_NO;
    }

    gateway_endpoint_request_t req = {.method = method,
                                      .path = url,
                                      .body = context->body_buf,
                                      .body_len = context->body_len,
                                      .user_data = matched->user_data};

    gateway_endpoint_response_t resp = {.status_code = 500,
                                        .content_type = "application/json",
                                        .body = NULL,
                                        .body_len = 0};

    int handler_ret = matched->handler(&req, &resp);

    struct MHD_Response *response = NULL;
    int ret = MHD_NO;

    if (handler_ret == 0 && resp.body) {
        response = MHD_create_response_from_buffer(resp.body_len, (void *)resp.body,
                                                   MHD_RESPMEM_MUST_COPY);
        if (response) {
            MHD_add_response_header(response, "Content-Type", resp.content_type);
            gateway_apply_security_headers(response);
            gateway_apply_cors_headers(gateway, connection, response);
            ret = MHD_queue_response(connection, resp.status_code, response);
            MHD_destroy_response(response);
        }
        atomic_fetch_add(&gateway->requests_total, 1);
        atomic_fetch_add(&gateway->bytes_sent, resp.body_len);
    } else {
        const char *err_body = "{\"error\":\"Internal server error\"}";
        response = MHD_create_response_from_buffer(strlen(err_body), (void *)err_body,
                                                   MHD_RESPMEM_PERSISTENT);
        if (response) {
            MHD_add_response_header(response, "Content-Type", "application/json");
            gateway_apply_security_headers(response);
            gateway_apply_cors_headers(gateway, connection, response);
            ret = MHD_queue_response(connection, 500, response);
            MHD_destroy_response(response);
        }
        atomic_fetch_add(&gateway->requests_failed, 1);
    }

    AIRY_FREE(resp.body);

    return ret;
}

/**
  * @brief HTTP request entry point
 *
  * Processing flow (4 phases):
  * Phase 1: initialize the request context (first call)
  * Phase 2: receive the POST body
  * Phase 3: handle the complete JSON-RPC request
  * Phase 4: route to other endpoints (OPTIONS/GET, etc.)
 */
int handle_http_request(void *cls, struct MHD_Connection *connection, const char *url,
                        const char *method, const char *version __attribute__((unused)),
                        const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    http_gateway_t *gateway = (http_gateway_t *)cls;
    http_request_context_t *context = (http_request_context_t *)*con_cls;

    if (gateway->rate_limiter) {
        const char *client_ip =
            MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Forwarded-For");
        if (!client_ip) {
            client_ip = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Real-IP");
        }
        if (!client_ip) {
            const union MHD_ConnectionInfo *cinfo =
                MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
            const struct sockaddr *addr =
                cinfo ? (const struct sockaddr *)cinfo->client_addr : NULL;
            if (addr) {
                char ip_buf[64];
                if (addr->sa_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in *)addr)->sin_addr, ip_buf,
                              sizeof(ip_buf));
                    client_ip = ip_buf;
                } else if (addr->sa_family == AF_INET6) {
                    inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, ip_buf,
                              sizeof(ip_buf));
                    client_ip = ip_buf;
                }
            }
        }
        if (!client_ip) {
            client_ip = "_unresolved";
        }

        if (!gateway_rate_limiter_allow(gateway->rate_limiter, client_ip)) {

            const char *error_response =
                "{\"error\":{\"code\":-32004,\"message\":\"Rate limit exceeded\"}}";
            struct MHD_Response *response =
                MHD_create_response_from_buffer(strlen(error_response), (void *)error_response,
                                                MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Content-Type", "application/json");
            MHD_add_response_header(response, "Server", "AgentRT-gateway/1.0");
            gateway_apply_security_headers(response);
            gateway_apply_cors_headers(gateway, connection, response);
            int ret = MHD_queue_response(connection, 429, response);
            MHD_destroy_response(response);
            return ret;
        }
    }

    if (!context) {
        context = AIRY_CALLOC(1, sizeof(http_request_context_t));
        if (!context) {
            return MHD_NO;
        }

        if (!gateway_is_url_safe(url)) {
            AIRY_FREE(context);
            const char *error_response =
                "{\"error\":{\"code\":-32002,\"message\":\"Invalid URL path\"}}";
            struct MHD_Response *response =
                MHD_create_response_from_buffer(strlen(error_response), (void *)error_response,
                                                MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Content-Type", "application/json");
            gateway_apply_security_headers(response);
            gateway_apply_cors_headers(gateway, connection, response);
            int ret = MHD_queue_response(connection, 400, response);
            MHD_destroy_response(response);
            return ret;
        }

        context->method = method;
        context->url = url;
        context->start_time_ns = gateway_time_ns();
        *con_cls = context;

        return MHD_YES;
    }

    if (strcmp(method, "POST") == 0 && upload_data && *upload_data_size > 0) {
        /* P1 fix: MHD delivers large POST bodies in multiple chunks and
         * REUSES the upload_data buffer between chunks. Only remembering the
         * last chunk's pointer truncates the body; accumulate into our own
         * buffer instead. */
        if (context->body_len + *upload_data_size > gateway->max_request_size) {
            return handle_request_too_large(gateway, connection, context,
                                            context->body_len + *upload_data_size);
        }

        if (context->body_len + *upload_data_size > context->body_cap) {
            size_t new_cap = context->body_cap == 0 ? 4096 : context->body_cap;
            while (new_cap < context->body_len + *upload_data_size) {
                new_cap *= 2;
            }
            char *nb = AIRY_REALLOC(context->body_buf, new_cap);
            if (!nb) {
                return MHD_NO;
            }
            context->body_buf = nb;
            context->body_cap = new_cap;
        }
        AIRY_MEMCPY(context->body_buf + context->body_len, upload_data, *upload_data_size);
        context->body_len += *upload_data_size;

        *upload_data_size = 0;
        return MHD_YES;
    }

    /* Stage 3: dispatch the complete request - both JSON-RPC and raw non-JSON-RPC bodies.
     * Streaming (SSE) endpoints are excluded: their response is a continuous event stream
     * handled directly by the route handler, not a one-shot JSON reply (SSoT via route table). */
    if (strcmp(method, "POST") == 0 && !is_streaming_route(method, url) &&
        (context->json_request || (context->body_buf && context->body_len > 0))) {
        if (!context->json_request && context->body_buf && context->body_len > 0) {
            if (parse_json_request(gateway, context, context->body_buf, context->body_len) != 0) {
                return handle_parse_error(gateway, connection, context, context->body_len);
            }
        }
        return handle_post_jsonrpc(gateway, connection, context);
    }

    int dynamic_ret = handle_dynamic_endpoint_route(gateway, connection, context, method, url);
    if (dynamic_ret != MHD_NO) {
        return dynamic_ret;
    }

    int (*route_handler)(http_gateway_t *, struct MHD_Connection *, http_request_context_t *) =
        find_http_route(method, url);

    if (route_handler) {
        return route_handler(gateway, connection, context);
    }

    return handle_not_found(gateway, connection, context);
}
