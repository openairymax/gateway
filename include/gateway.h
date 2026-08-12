/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file gateway.h
 * @brief AgentRT gateway unified public interface.
 *
 * The gateway layer only performs protocol translation, converting external
 * requests into agentrt/atoms/syscall calls.
 *
 * Architecture: agentrt/daemons/gateway_d/ -> agentrt/gateway/ -> atoms/syscall/
 *
 * Design principles (per ARCHITECTURAL_PRINCIPLES.md):
 *   K-1 minimal core: the gateway only translates protocols, zero business logic
 *   K-2 contract interfaces: all APIs carry full Doxygen contracts
 *   S-2 layered decomposition: strict layering, no cross-layer access
 */

/* @owner: team-B */
#ifndef AIRY_RT_GATEWAY_H
#define AIRY_RT_GATEWAY_H

#include "airy_rt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
  * @brief Gateway-specific error codes (extend the AgentRT standard error codes)
 *
 * @note Gateway APIs return both airy_err_t and gateway_error_t; the latter
 *       covers gateway-specific error scenarios.
 */
typedef enum {
    GATEWAY_SUCCESS = 0,
    GATEWAY_ERROR_INVALID = -1,
    GATEWAY_ERROR_MEMORY = -2,
    GATEWAY_ERROR_IO = -3,
    GATEWAY_ERROR_TIMEOUT = -4,
    GATEWAY_ERROR_CLOSED = -5,
    GATEWAY_ERROR_PROTOCOL = -6
} gateway_error_t;


/**
  * @brief Gateway type enumeration
 */
typedef enum { GATEWAY_TYPE_HTTP = 0, GATEWAY_TYPE_WS, GATEWAY_TYPE_STDIO } gateway_type_t;


/**
  * @brief Opaque gateway handle
 *
 * @note Internally holds an ops table + impl pointer + type tag.
 */
typedef struct gateway gateway_t;


/**
  * @brief Request handler callback function type
 *
 * Inject a custom request handler when external logic is needed. Once set,
 * the gateway invokes this callback first on incoming requests; otherwise
 * it falls back to the default syscall routing.
 *
 * @param[in] request_json Request JSON string (JSON-RPC 2.0), ownership not transferred
 * @param[out] response_json Output response JSON string, allocated by the callback, caller frees
 * @param[in] user_data User data passed at gateway_set_handler() time
 * @return 0 on success, non-zero on failure (negative gateway_error_t)
 *
 * @threadsafe The callback may be invoked from multiple threads (HTTP/WebSocket)
 * @ownership response_json must be malloc/strdup'd by the callback; the caller frees it
 *
 * @see gateway_set_handler()
 */
typedef int (*gateway_request_handler_t)(const char *request_json, char **response_json,
                                         void *user_data);


/**
  * @brief Create an HTTP gateway instance
 *
 * Create an HTTP gateway on libmicrohttpd, listening on the given address and
 * port, receiving JSON-RPC 2.0 POST requests and translating them to syscalls.
 *
 * @param[in] host Listen address (e.g. "127.0.0.1", "0.0.0.0"), must not be NULL
 * @param[in] port Listen port (e.g. 8080)
 * @return Gateway handle, or NULL on failure (OOM or invalid parameters)
 */
gateway_t *gateway_http_create(const char *host, uint16_t port);

/**
  * @brief Create a WebSocket gateway instance
 *
 * Create a WebSocket gateway on libwebsockets, supporting bidirectional RPC.
 *
 * @param[in] host Listen address, must not be NULL
 * @param[in] port Listen port
 * @return Gateway handle, or NULL on failure
 *
 * @ownership Caller must release via gateway_destroy()
 * @threadsafe yes
 * @since 1.0.0
 */
gateway_t *gateway_ws_create(const char *host, uint16_t port);

/**
  * @brief Create a stdio gateway instance
 *
 * Create a stdio-based command-line gateway for CLI/pipe scenarios. After
 * start(), it runs a blocking interactive loop until the user types "exit".
 *
 * @return Gateway handle, or NULL on failure
 *
 * @ownership Caller must release via gateway_destroy()
 * @threadsafe yes (single-threaded blocking after start())
 * @since 1.0.0
 *
 * @note The stdio gateway's start() blocks and runs the REPL loop on the current thread
 */
gateway_t *gateway_stdio_create(void);

/**
  * @brief Destroy the gateway instance and free all resources
 *
 * Stops a running gateway automatically, then frees all associated resources.
 * Silently ignores NULL input.
 *
 * @param[in] gw Gateway handle (may be NULL, silently ignored)
 *
 * @ownership Transfers ownership of the handle; gw must not be used afterwards
 * @threadsafe no, the caller must serialize
 * @since 1.0.0
 */
void gateway_destroy(gateway_t *gw);


/**
  * @brief Start the gateway
 *
 * Start listening/accepting input. HTTP/WS gateways start non-blocking
 * (background thread); the stdio gateway starts blocking (REPL loop).
 *
 * @param[in] gw Gateway handle
 * @return AIRY_SUCCESS on success
 * @return AIRY_EINVAL invalid parameters
 * @return AIRY_EBUSY port busy or resource busy
 *
 * @pre The gateway was created via gateway_http/ws/stdio_create
 * @post The gateway is running; check with gateway_is_running()
 * @threadsafe yes
 * @since 1.0.0
 */
int gateway_start(gateway_t *gw);

/**
  * @brief Stop the gateway
 *
 * Gracefully stop the gateway, waiting for in-flight requests to finish.
 * Silently ignores NULL or already-stopped gateways.
 *
 * @param[in] gw Gateway handle (may be NULL)
 * @return AIRY_SUCCESS on success or silent ignore
 *
 * @post The gateway stops accepting new connections; is_running() returns false
 * @threadsafe yes
 * @since 1.0.0
 */
int gateway_stop(gateway_t *gw);

/**
  * @brief Set a custom request handler callback
 *
 * Once set, the gateway invokes this callback first on incoming requests.
 * If the callback returns non-zero, its response_json is used as the response;
 * if it returns 0 without setting response_json, the default syscall routing runs.
 *
 * @param[in] gw Gateway handle
 * @param[in] handler Callback (NULL clears the custom handler)
 * @param[in] user_data User data passed to the callback
 * @return AIRY_SUCCESS on success
 * @return AIRY_EINVAL invalid parameters
 *
 * @threadsafe yes (atomic set)
 * @since 1.0.0
 */
int gateway_set_handler(gateway_t *gw, gateway_request_handler_t handler, void *user_data);


/**
  * @brief Endpoint request structure (for dynamic endpoint registration)
 */
typedef struct gateway_endpoint_request {
    const char *method;
    const char *path;
    const char *body;
    size_t body_len;
    void *user_data;
} gateway_endpoint_request_t;

/**
  * @brief Endpoint response structure (for dynamic endpoint registration)
 *
 * The handler allocates body (strdup/strndup); the bridge layer frees it.
 * content_type points to a static string literal; the bridge does not free it.
 */
typedef struct gateway_endpoint_response {
    int status_code;
    const char *content_type;
    char *body;
    size_t body_len;
} gateway_endpoint_response_t;

/**
  * @brief Dynamic endpoint handler callback type
 *
 * @param[in] req Request information
 * @param[out] resp Response information (filled by the handler)
 * @return 0 on success, non-zero on failure
 *
 * @ownership resp->body is allocated by the handler (malloc/strdup); the bridge frees it
 */
typedef int (*gateway_endpoint_handler_t)(const gateway_endpoint_request_t *req,
                                          gateway_endpoint_response_t *resp);


/**
  * @brief Get the gateway type
 *
 * @param[in] gw Gateway handle (may be NULL)
 * @return Gateway type enumeration; NULL returns GATEWAY_TYPE_HTTP (default)
 *
 * @threadsafe yes
 * @since 1.0.0
 */
gateway_type_t gateway_get_type(gateway_t *gw);

/**
  * @brief Check whether the gateway is running
 *
 * @param[in] gw Gateway handle (may be NULL)
 * @return true if running
 * @return false if stopped or parameters invalid
 *
 * @threadsafe yes (atomic read)
 * @since 1.0.0
 */
bool gateway_is_running(gateway_t *gw);

/**
  * @brief Get gateway statistics
 *
 * Returns statistics as a JSON string: request counts, bytes, errors, etc.
 *
 * @param[in] gw Gateway handle
 * @param[out] out_json Output JSON string; caller must free()
 * @return AIRY_SUCCESS on success
 * @return AIRY_EINVAL invalid parameters
 *
 * @ownership out_json is allocated by the function; the caller must free()
 * @threadsafe yes (snapshot read)
 * @since 1.0.0
 *
 * @code
 * char* stats = NULL;
 * if (gateway_get_stats(gw, &stats) == AIRY_SUCCESS) {
 *     printf("Stats: %s\n", stats);
 *     free(stats);
 * }
 * @endcode
 */
int gateway_get_stats(gateway_t *gw, char **out_json);

/**
  * @brief Get the gateway name
 *
 * @param[in] gw Gateway handle (may be NULL)
 * @return Gateway name string (e.g. "HTTP Gateway"); NULL returns "unknown"
 *
 * @threadsafe yes
 * @since 1.0.0
 */
const char *gateway_get_name(gateway_t *gw);

/**
  * @brief Register a dynamic HTTP endpoint
 *
 * Registers a custom endpoint handler with the gateway's HTTP server.
 * Registered endpoints take precedence over same-path entries in the static
 * route table. Only the HTTP gateway supports endpoint registration; other
 * types return AIRY_EINVAL.
 *
 * @param[in] gw Gateway handle
 * @param[in] method HTTP method (e.g. "GET", "POST"), must not be NULL
 * @param[in] path URL path (e.g. "/metrics"), must not be NULL
 * @param[in] handler Endpoint handler callback, must not be NULL
 * @param[in] user_data User data passed to the callback (may be NULL)
 * @return AIRY_SUCCESS on success
 * @return AIRY_EINVAL invalid parameters or unsupported gateway type
 * @return AIRY_ENOMEM out of memory
 *
 * @note Should be called before gateway_start(); runtime registration must ensure thread safety
 * @threadsafe The registration itself is safe, but be careful when racing request handling
 * @since 0.1.0
 */
int gateway_register_endpoint(gateway_t *gw, const char *method, const char *path,
                              gateway_endpoint_handler_t handler, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_H */
