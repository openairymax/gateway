// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file http_gateway.c
 * @brief HTTP gateway implementation - libmicrohttpd integration.
 *
 * Implements JSON-RPC 2.0 protocol handling, communicating with the kernel
 * through the syscall interface. The gateway only translates protocols and
 * contains no business logic.
 */

// @owner: team-B
#include "http_gateway.h"

#include "../../../commons/utils/error/include/error.h"
#include "../utils/gateway_protocol_handler.h"
#include "../utils/gateway_rate_limiter.h"
#include "../utils/gateway_rpc_handler.h"
#include "../utils/gateway_utils.h"
#include "../utils/jsonrpc.h"
#include "../utils/syscall_router.h"
#include "error.h"
#include "airy_memory.h"

#ifdef GATEWAY_HAS_HTTP

#include <microhttpd.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atomic_compat.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/*
 * time_ns() migrated to gateway_utils.h (gateway_time_ns); this file
 * uses gateway_time_ns() consistently.
 */

void gateway_apply_security_headers(struct MHD_Response *response)
{
    if (!response)
        return;

    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "X-Frame-Options", "DENY");
    MHD_add_response_header(response, "X-XSS-Protection", "1; mode=block");
    MHD_add_response_header(response, "Strict-Transport-Security",
                            "max-age=31536000; includeSubDomains");
    MHD_add_response_header(response, "Cache-Control", "no-store, no-cache, must-revalidate");
    MHD_add_response_header(response, "Pragma", "no-cache");
}

/**
  * @brief Check whether Origin is in the CORS whitelist
  * @param gateway HTTP gateway instance
  * @param origin Origin header value
  * @return true if allowed, false if rejected
 */
static bool is_cors_origin_allowed(const http_gateway_t *gateway, const char *origin)
{
    if (!origin || !gateway)
        return false;

    if (gateway->cors.allow_all_origins) {
        return true;
    }

    for (size_t i = 0; i < gateway->cors.allowed_origins_count; i++) {
        if (gateway->cors.allowed_origins[i] &&
            strcmp(origin, gateway->cors.allowed_origins[i]) == 0) {
            return true;
        }
    }

    return false;
}

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
                                struct MHD_Response *response)
{
    if (!gateway || !connection || !response)
        return;

    const char *origin = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Origin");
    if (is_cors_origin_allowed(gateway, origin)) {
        MHD_add_response_header(response, "Access-Control-Allow-Origin", origin);

        if (gateway->cors.allowed_methods) {
            MHD_add_response_header(response, "Access-Control-Allow-Methods",
                                    gateway->cors.allowed_methods);
        }

        if (gateway->cors.allowed_headers) {
            MHD_add_response_header(response, "Access-Control-Allow-Headers",
                                    gateway->cors.allowed_headers);
        }

        if (gateway->cors.max_age > 0) {
            char max_age_str[16];
            snprintf(max_age_str, sizeof(max_age_str), "%d", gateway->cors.max_age);
            MHD_add_response_header(response, "Access-Control-Max-Age", max_age_str);
        }
    }
}

/**
  * @brief Build an HTTP response (CORS-safe variant)
  * @param gateway HTTP gateway instance
  * @param connection MHD connection object
  * @param status_code HTTP status code
  * @param content Response content
  * @param content_len Content length
 * @return MHD response object
 */
struct MHD_Response *create_http_response_ex(http_gateway_t *gateway,
                                             struct MHD_Connection *connection,
                                             int status_code __attribute__((unused)),
                                             const char *content, size_t content_len)
{

    struct MHD_Response *response =
        MHD_create_response_from_buffer(content_len, (void *)content, MHD_RESPMEM_MUST_COPY);

    if (!response) {
        return NULL;
    }

    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_add_response_header(response, "Server", "AgentRT-gateway/1.0");

    gateway_apply_security_headers(response);
    gateway_apply_cors_headers(gateway, connection, response);

    return response;
}

/**
  * @brief Build an HTTP response (legacy-compatible variant)
  * @param status_code HTTP status code
  * @param content Response content
  * @param content_len Content length
 * @return MHD response object
 * @deprecated Use create_http_response_ex() for CORS-safe handling
 */
struct MHD_Response *create_http_response(int status_code, const char *content, size_t content_len)
{
    struct MHD_Response *response =
        MHD_create_response_from_buffer(content_len, (void *)content, MHD_RESPMEM_MUST_COPY);

    if (!response) {
        return NULL;
    }

    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_add_response_header(response, "Server", "AgentRT-gateway/1.0");

    gateway_apply_security_headers(response);

    return response;
}

/**
  * @brief Parse a JSON request body
 * @param gateway Gateway instance
  * @param context Request context
  * @param data Request body data
  * @param size Data size
  * @return 0 on success, non-zero on failure
 */
int parse_json_request(http_gateway_t *gateway, http_request_context_t *context, const char *data,
                       size_t size)
{
    if (!data || size == 0) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "parse_json_request: parse error");
        return AIRY_ERR_UNKNOWN;
    }

    if (size > gateway->max_request_size) {

        atomic_fetch_add(&gateway->requests_failed, 1);
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "if: failed");
        return AIRY_ERR_UNKNOWN;
    }

    /* P0: MHD upload_data is not '\0'-terminated; cJSON_Parse would overrun;
      * parse by size with cJSON_ParseWithLength instead */
    context->json_request = cJSON_ParseWithLength(data, size);
    if (!context->json_request) {
        /* Non-JSON body: not a parse error; keep upload_data for the handler fallback
          * (external protocols such as OpenAI/MCP/A2A are detected by gateway_d's entry) */
        return 0;
    }

    if (gw_jsonrpc_validate_request(context->json_request) != 0) {
        /* Valid JSON but not JSON-RPC (e.g. OpenAI chat/completions):
          * keep upload_data for the protocol entry handler to route */
        cJSON_Delete(context->json_request);
        context->json_request = NULL;
        return 0;
    }

    return 0;
}
/**
  * @brief Request handler adapter - converts the public callback signature to the internal one
 *
  * Public signature: (const char* request_json, char** response_json, void* user_data) -> int
  * Internal signature: (void* request, void* user_data) -> char*
 *
  * Stores the public-style callback internally and adapts it on invocation。
 */
typedef struct {
    int (*public_handler)(const char *, char **, void *);
    void *user_data;
} http_handler_adapter_t;

/**
  * @brief Internal callback wrapper (matches the internal gateway_request_handler_t signature)
  * @param request cJSON request object
  * @param user_data Pointer to http_handler_adapter_t
  * @return JSON response string (caller frees), or NULL
 */
static char *__attribute__((used)) http_handler_adapter(void *request, void *user_data)
{
    http_handler_adapter_t *adapter = (http_handler_adapter_t *)user_data;
    if (!adapter || !adapter->public_handler)
        return NULL;

    char *request_json = cJSON_Print((cJSON *)request);
    if (!request_json)
        return NULL;

    char *response_json = NULL;
    int ret = adapter->public_handler(request_json, &response_json, adapter->user_data);
    AIRY_FREE(request_json);

    if (ret != 0 || !response_json) {
        return NULL;
    }

    return response_json;
}

typedef struct {
    gateway_internal_handler_t internal_handler;
    void *internal_data;
} internal_to_public_adapter_t;

static int internal_handler_public_wrapper(const char *request_json, char **response_json,
                                           void *user_data)
{
    internal_to_public_adapter_t *adapter = (internal_to_public_adapter_t *)user_data;
    if (!adapter || !adapter->internal_handler) {
        *response_json = NULL;
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "if: null pointer");
        return AIRY_ERR_UNKNOWN;
    }
    char *resp = adapter->internal_handler((void *)request_json, adapter->internal_data);
    if (resp) {
        *response_json = resp;
        return 0;
    }
    *response_json = NULL;
    airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__, "if: null pointer");
    return AIRY_ERR_NULL_POINTER;
}

/**
  * @brief Handle a JSON-RPC request (via the unified RPC handler)
 * @param gateway Gateway instance
  * @param context Request context
  * @return JSONResponse string
 */
char *handle_jsonrpc_request(http_gateway_t *gateway, http_request_context_t *context)
{
    rpc_result_t result;

    if (gateway->protocol_handler && context->upload_data && context->upload_data_size > 0) {
        internal_to_public_adapter_t adapter = {.internal_handler = gateway->handler,
                                                .internal_data = gateway->handler_data};
        result = gateway_protocol_handle_request(gateway->protocol_handler, context->upload_data,
                                                 context->upload_data_size, AIRY_PROTOCOL_COUNT,
                                                 internal_handler_public_wrapper, &adapter);
    } else if (context->json_request) {
        internal_to_public_adapter_t adapter = {.internal_handler = gateway->handler,
                                                .internal_data = gateway->handler_data};
        result = gateway_rpc_handle_request(context->json_request, internal_handler_public_wrapper,
                                            &adapter);
    } else if (context->upload_data && context->upload_data_size > 0 && gateway->handler) {
        /* Raw non-JSON-RPC body (OpenAI/MCP/A2A): pass the HTTP context
         * (method/path + NUL-terminated body copy) to the protocol entry
         * handler for detection and routing (translated uniformly by
         * gateway_d, D2). MHD upload_data is not NUL-terminated, so a
         * terminated copy is required before any strstr/cJSON parsing. */
        char *body_copy = (char *)AIRY_MALLOC(context->upload_data_size + 1);
        if (!body_copy) {
            result.error_code = -32603;
            result.error_message = AIRY_STRDUP("Out of memory");
        } else {
            AIRY_MEMCPY(body_copy, context->upload_data, context->upload_data_size);
            body_copy[context->upload_data_size] = '\0';

            gateway_http_request_t http_req = {0};
            http_req.magic = GATEWAY_HTTP_REQUEST_MAGIC;
            http_req.method = context->method ? context->method : "POST";
            http_req.path = context->url;
            http_req.body = body_copy;
            http_req.body_len = context->upload_data_size;

            char *resp = gateway->handler(&http_req, gateway->handler_data);
            AIRY_MEMSET(&result, 0, sizeof(result));
            if (resp) {
                result.response_json = resp;
            } else {
                result.error_code = -32603;
                result.error_message = AIRY_STRDUP("Protocol handler failed");
            }
            AIRY_FREE(body_copy);
        }
    } else {

        return jsonrpc_create_error_response(NULL, -32600, "Invalid request", NULL);
    }

    if (result.error_code != 0 || !result.response_json) {

        char *error_resp = result.response_json ?
                               result.response_json :
                               jsonrpc_create_error_response(NULL, -32603, "Internal error", NULL);
        if (result.response_json) {
            result.response_json = NULL;
        }
        gateway_rpc_free(&result);
        return error_resp;
    }

    char *success_resp = result.response_json;
    result.response_json = NULL;
    gateway_rpc_free(&result);

    return success_resp;
}

static void http_request_completed_callback(
    void *cls __attribute__((unused)), struct MHD_Connection *connection __attribute__((unused)),
    void **con_cls, enum MHD_RequestTerminationCode toe __attribute__((unused)))
{
    if (con_cls && *con_cls) {
        http_request_context_t *ctx = (http_request_context_t *)*con_cls;
        if (ctx->json_request) {
            cJSON_Delete(ctx->json_request);
            ctx->json_request = NULL;
        }
        AIRY_FREE(ctx);
        *con_cls = NULL;
    }
}

static airy_err_t http_gateway_start(void *gateway_impl)
{
    http_gateway_t *gateway = (http_gateway_t *)gateway_impl;

    unsigned int conn_limit = gateway->connection_limit > 0 ? gateway->connection_limit : 1000;
    unsigned int conn_timeout = gateway->connection_timeout > 0 ? gateway->connection_timeout : 30;

    const char *env_conn = getenv("GATEWAY_HTTP_CONN_LIMIT");
    const char *env_timeout = getenv("GATEWAY_HTTP_TIMEOUT");
    if (env_conn) {
        unsigned long v = strtoul(env_conn, NULL, 10);
        if (v > 0)
            conn_limit = (unsigned int)v;
    }
    if (env_timeout) {
        unsigned long v = strtoul(env_timeout, NULL, 10);
        if (v > 0)
            conn_timeout = (unsigned int)v;
    }

    /* HTTP 线程池大小（工业级目标：大型服务器吞吐可扩展）。
     * GATEWAY_HTTP_THREADS 显式覆盖；缺省按 CPU 核数自适应，
     * 下限 4、上限 32，避免在单核/低端设备上无谓开线程。 */
    unsigned int pool_size = 4;
    const char *env_threads = getenv("GATEWAY_HTTP_THREADS");
    if (env_threads) {
        unsigned long v = strtoul(env_threads, NULL, 10);
        if (v >= 1 && v <= 1024)
            pool_size = (unsigned int)v;
    } else {
        long nproc = 0;
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        nproc = (long)si.dwNumberOfProcessors;
#else
        nproc = sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (nproc >= 4)
            pool_size = (nproc > 32) ? 32 : (unsigned int)nproc;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    gateway->daemon =
        MHD_start_daemon(MHD_USE_EPOLL_INTERNAL_THREAD | MHD_USE_TURBO, gateway->port, NULL, NULL,
                         handle_http_request, gateway, MHD_OPTION_CONNECTION_LIMIT, conn_limit,
                         MHD_OPTION_CONNECTION_TIMEOUT, conn_timeout, MHD_OPTION_THREAD_POOL_SIZE,
                         pool_size, MHD_OPTION_NOTIFY_COMPLETED, http_request_completed_callback,
                         NULL, MHD_OPTION_END);
#pragma GCC diagnostic pop

    if (!gateway->daemon) {
        return AIRY_EBUSY;
    }

    atomic_store(&gateway->running, true);

    return AIRY_SUCCESS;
}
static void http_gateway_stop(void *gateway_impl)
{
    http_gateway_t *gateway = (http_gateway_t *)gateway_impl;

    atomic_store(&gateway->running, false);

    if (gateway->daemon) {
        MHD_stop_daemon(gateway->daemon);
        gateway->daemon = NULL;
    }
}
static void http_gateway_destroy(void *gateway_impl)
{
    http_gateway_t *gateway = (http_gateway_t *)gateway_impl;

    http_gateway_stop(gateway);

    if (gateway->handler_adapter) {
        AIRY_FREE(gateway->handler_adapter);
        gateway->handler_adapter = NULL;
    }
    gateway->handler = NULL;
    gateway->handler_data = NULL;

    if (gateway->host) {
        AIRY_FREE(gateway->host);
    }

    if (gateway->cors.allowed_methods) {
        AIRY_FREE(gateway->cors.allowed_methods);
    }
    if (gateway->cors.allowed_headers) {
        AIRY_FREE(gateway->cors.allowed_headers);
    }
    if (gateway->cors.allowed_origins) {
        for (size_t i = 0; i < gateway->cors.allowed_origins_count; i++) {
            if (gateway->cors.allowed_origins[i]) {
                AIRY_FREE(gateway->cors.allowed_origins[i]);
            }
        }
        AIRY_FREE(gateway->cors.allowed_origins);
    }

    if (gateway->rate_limiter) {
        gateway_rate_limiter_destroy(gateway->rate_limiter);
    }

    if (gateway->protocol_handler) {
        gateway_protocol_handler_destroy(gateway->protocol_handler);
        gateway->protocol_handler = NULL;
    }

    if (gateway->dynamic_endpoints) {
        for (size_t i = 0; i < gateway->dynamic_endpoint_count; i++) {
            AIRY_FREE(gateway->dynamic_endpoints[i].method);
            AIRY_FREE(gateway->dynamic_endpoints[i].path);
        }
        AIRY_FREE(gateway->dynamic_endpoints);
        gateway->dynamic_endpoints = NULL;
    }
    gateway->dynamic_endpoint_count = 0;
    gateway->dynamic_endpoint_capacity = 0;

    AIRY_FREE(gateway);
}
static const char *http_gateway_get_name(void *gateway_impl __attribute__((unused)))
{
    return "HTTP Gateway";
}
static airy_err_t http_gateway_get_stats(void *gateway_impl, char **out_json)
{
    http_gateway_t *gateway = (http_gateway_t *)gateway_impl;
    if (!gateway || !out_json)
        return AIRY_EINVAL;

    cJSON *stats = cJSON_CreateObject();
    if (!stats)
        return AIRY_ENOMEM;
    cJSON_AddNumberToObject(stats, "requests_total", (double)atomic_load(&gateway->requests_total));
    cJSON_AddNumberToObject(stats, "requests_failed",
                            (double)atomic_load(&gateway->requests_failed));
    cJSON_AddNumberToObject(stats, "bytes_received", (double)atomic_load(&gateway->bytes_received));
    cJSON_AddNumberToObject(stats, "bytes_sent", (double)atomic_load(&gateway->bytes_sent));

    char *json_str = cJSON_Print(stats);
    cJSON_Delete(stats);

    if (!json_str)
        return AIRY_ENOMEM;
    *out_json = json_str;
    return AIRY_SUCCESS;
}

/**
  * @brief Check whether the HTTP gateway is running
  * @param gateway_impl Gateway implementation pointer
  * @return true if running, false if stopped or invalid
 */
static bool http_gateway_is_running(void *gateway_impl)
{
    http_gateway_t *gateway = (http_gateway_t *)gateway_impl;
    if (!gateway)
        return false;
    return atomic_load(&gateway->running);
}

/**
  * @brief Set the request handler callback
 *
  * Two callback modes are supported:
  * 1. Internal mode: pass a (void*, void*) -> char* callback directly
  * 2. Public mode (recommended): pass via gateway_set_handler(),
  *    an adapter auto-converts the public signature to the internal one
 */
static airy_err_t http_gateway_set_handler(void *gateway_impl, gateway_internal_handler_t handler,
                                           void *user_data)
{
    http_gateway_t *gateway = (http_gateway_t *)gateway_impl;
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

static const gateway_ops_t http_gateway_ops = {.start = http_gateway_start,
                                               .stop = http_gateway_stop,
                                               .destroy = http_gateway_destroy,
                                               .get_name = http_gateway_get_name,
                                               .get_stats = http_gateway_get_stats,
                                               .is_running = http_gateway_is_running,
                                               .set_handler = http_gateway_set_handler};

gateway_t *http_gateway_create(const char *host, uint16_t port)
{
    if (!host) {
        return NULL;
    }

    http_gateway_t *gateway = AIRY_CALLOC(1, sizeof(http_gateway_t));
    if (!gateway) {
        return NULL;
    }

    gateway->port = port;
    gateway->host = AIRY_STRDUP(host);
    gateway->handler_adapter = NULL;
    gateway->handler = NULL;
    gateway->handler_data = NULL;

    if (!gateway->host) {
        AIRY_FREE(gateway);
        return NULL;
    }

    atomic_init(&gateway->running, false);
    atomic_init(&gateway->requests_total, 0);
    atomic_init(&gateway->requests_failed, 0);
    atomic_init(&gateway->bytes_received, 0);
    atomic_init(&gateway->bytes_sent, 0);

    gateway->max_request_size = 1 * 1024 * 1024; /* 1MB */
    const char *env_max_size = getenv("GATEWAY_MAX_REQUEST_SIZE");
    if (env_max_size) {
        long size = strtol(env_max_size, NULL, 10);
        if (size > 0 && size <= 100 * 1024 * 1024) {
            gateway->max_request_size = (size_t)size;
        }
    }

    gateway->cors.allow_all_origins = false;
    gateway->cors.allowed_origins = NULL;
    gateway->cors.allowed_origins_count = 0;
    gateway->cors.allowed_methods = AIRY_STRDUP("POST, GET, OPTIONS");
    gateway->cors.allowed_headers = AIRY_STRDUP("Content-Type, Authorization");
    gateway->cors.max_age = 3600;

    const char *cors_mode = getenv("GATEWAY_CORS_MODE");
    if (cors_mode && strcmp(cors_mode, "dev") == 0) {
        gateway->cors.allow_all_origins = true;
    }

    const char *cors_origins = getenv("GATEWAY_CORS_ORIGINS");
    if (cors_origins && !gateway->cors.allow_all_origins) {

        char *origins_copy = AIRY_STRDUP(cors_origins);
        if (origins_copy) {
            size_t count = 1;
            for (char *p = origins_copy; *p; p++) {
                if (*p == ',')
                    count++;
            }

            if (count <= SIZE_MAX / sizeof(char *)) {
                gateway->cors.allowed_origins = (char **)airy_malloc_array(count, sizeof(char *));
                if (gateway->cors.allowed_origins) {
                    char *saveptr = NULL;
                    char *token = strtok_r(origins_copy, ",", &saveptr);
                    size_t i = 0;
                    while (token && i < count) {
                        gateway->cors.allowed_origins[i++] = AIRY_STRDUP(token);
                        token = strtok_r(NULL, ",", &saveptr);
                    }
                    gateway->cors.allowed_origins_count = i;
                }
            }
            AIRY_FREE(origins_copy);
        }
    }

    gateway->rate_limiter = NULL;
    const char *rate_limit_enabled = getenv("GATEWAY_RATE_LIMIT_ENABLED");
    if (rate_limit_enabled && strcmp(rate_limit_enabled, "true") == 0) {
        gateway_rate_limit_config_t rl_config;
        gateway_rate_limiter_get_default_config(&rl_config);
        rl_config.enabled = true;

        const char *rps = getenv("GATEWAY_RATE_LIMIT_RPS");
        if (rps) {
            rl_config.requests_per_second = (uint32_t)strtol(rps, NULL, 10);
        }

        const char *rpm = getenv("GATEWAY_RATE_LIMIT_RPM");
        if (rpm) {
            rl_config.requests_per_minute = (uint32_t)strtol(rpm, NULL, 10);
        }

        gateway->rate_limiter = gateway_rate_limiter_create(&rl_config);
    }

    /* Initialize the multi-protocol handler (off by default)
     *
      * Protocol detection/translation lives in gateway_d's adapter (D2); avoid a double translation layer here
      * Set GATEWAY_PROTOCOL_HANDLER=true only to enable gateway-level conversion. */
    gateway->protocol_handler = NULL;
    const char *proto_handler_env = getenv("GATEWAY_PROTOCOL_HANDLER");
    if (proto_handler_env && strcmp(proto_handler_env, "true") == 0) {
        gateway->protocol_handler = gateway_protocol_handler_create(NULL);
        if (!gateway->protocol_handler) {
        }
    }

    gateway_t *gw = AIRY_MALLOC(sizeof(gateway_t));
    if (!gw) {
        AIRY_FREE(gateway->cors.allowed_methods);
        AIRY_FREE(gateway->cors.allowed_headers);
        if (gateway->cors.allowed_origins) {
            for (size_t i = 0; i < gateway->cors.allowed_origins_count; i++) {
                AIRY_FREE(gateway->cors.allowed_origins[i]);
            }
            AIRY_FREE(gateway->cors.allowed_origins);
        }
        if (gateway->protocol_handler) {
            gateway_protocol_handler_destroy(gateway->protocol_handler);
        }
        if (gateway->rate_limiter) {
            gateway_rate_limiter_destroy(gateway->rate_limiter);
        }
        AIRY_FREE(gateway->host);
        AIRY_FREE(gateway);
        return NULL;
    }

    gw->ops = &http_gateway_ops;
    gw->impl = gateway;
    gw->type = GATEWAY_TYPE_HTTP;

    return gw;
}

int http_gateway_register_endpoint(http_gateway_t *gateway, const char *method, const char *path,
                                   gateway_endpoint_handler_t handler, void *user_data)
{
    if (!gateway || !method || !path || !handler) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "http_gateway_register_endpoint: failed");
        return AIRY_ERR_UNKNOWN;
    }

    if (gateway->dynamic_endpoint_count >= gateway->dynamic_endpoint_capacity) {
        size_t new_cap =
            gateway->dynamic_endpoint_capacity == 0 ? 8 : gateway->dynamic_endpoint_capacity * 2;
        http_dynamic_endpoint_t *new_arr =
            AIRY_REALLOC(gateway->dynamic_endpoints, new_cap * sizeof(http_dynamic_endpoint_t));
        if (!new_arr) {
            return AIRY_ERR_INVALID_PARAM;
        }
        gateway->dynamic_endpoints = new_arr;
        gateway->dynamic_endpoint_capacity = new_cap;
    }

    http_dynamic_endpoint_t *slot = &gateway->dynamic_endpoints[gateway->dynamic_endpoint_count];
    slot->method = AIRY_STRDUP(method);
    slot->path = AIRY_STRDUP(path);
    if (!slot->method || !slot->path) {
        AIRY_FREE(slot->method);
        AIRY_FREE(slot->path);
        return AIRY_ERR_INVALID_PARAM;
    }
    slot->handler = handler;
    slot->user_data = user_data;

    gateway->dynamic_endpoint_count++;

    return 0;
}

#endif /* GATEWAY_HAS_HTTP */
#ifndef GATEWAY_HAS_HTTP

gateway_t *http_gateway_create(const char *host __attribute__((unused)),
                               uint16_t port __attribute__((unused)))
{
    return NULL;
}

#endif /* !GATEWAY_HAS_HTTP */
