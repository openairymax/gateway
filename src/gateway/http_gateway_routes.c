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
#include "syscalls.h"

#include <microhttpd.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif
#include <stdlib.h>
#include <string.h>

/* MHD header iterator callback (same as http_gateway.c) */
static int parse_headers(void *cls __attribute__((unused)),
                         enum MHD_ValueKind kind __attribute__((unused)),
                         const char *key __attribute__((unused)),
                         const char *value __attribute__((unused)))
{
    return MHD_YES;
}

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

    uint64_t response_time_ns = gateway_time_ns() - context->start_time_ns;
    AIRY_LOG_DEBUG("请求处理耗时: %lu ns", response_time_ns);

    atomic_fetch_add(&gateway->requests_total, 1);
    atomic_fetch_add(&gateway->bytes_received, context->upload_data_size);
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

    const char *health_json = "{\"status\":\"healthy\",\"service\":\"gateway\"}";
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
  * @brief SSE streaming chat endpoint constants
 *
  * The gateway proxies llm_d complete_stream. Clients POST
  * OpenAI messages format (or the simplified JSON-RPC agent.run form); the gateway connects to llm_d
  * and forwards incremental text chunks as SSE events for TUI rendering.
  * Design: LLM-direct streaming only, no tool loop / no think_d (Claude Code style).
 */
#define GW_SSE_CHAT_PATH "/api/v1/chat/stream"
#define GW_SSE_DEFAULT_MODEL "deepseek-v4-flash"
#define GW_SSE_RECV_TIMEOUT_S 30
#define GW_SSE_BLOCK_SIZE 1024
#define GW_SSE_DONE_EVENT "data: [DONE]\n\n"

/**
  * @brief SSE streaming response callback context
 *
  * cls for MHD_create_response_from_callback: holds the llm_d socket fd and an end flag.
  * Freed by MHD's free_cb (gw_sse_content_free): closes the fd and AIRY_FREE.
 */
typedef struct {
    int fd;
    int done;
} gw_sse_ctx_t;

/**
  * @brief Resolve the llm_d socket path: env AIRY_LLM_SOCK -> airy_runtime_dir()/llm.sock
 *
  * Same origin as gw_resolve_daemon_sock in gateway_business_handler.c:
  * airy_runtime_dir() resolves $AIRY_HOME/run, defaulting to ~/.airymaxrt/run.
  * (airy_runtime_dir_socket uses a static buffer, so splice the path here.)
 */
static void gw_sse_resolve_llm_sock(char *out, size_t out_size)
{
    const char *env = getenv("AIRY_LLM_SOCK");
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_size);
        return;
    }
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir) {
        snprintf(out, out_size, "%s/llm.sock", run_dir);
    } else {
        AIRY_STRNCPY_TERM(out, "llm.sock", out_size);
    }
}

/**
  * @brief Send a JSON error response (non-streaming failure path: 400/500/502)
 */
static int gw_sse_send_json_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                                  int status, const char *message)
{
    char err[256];
    int n = snprintf(err, sizeof(err), "{\"error\":{\"code\":%d,\"message\":\"%s\"}}", status,
                     message ? message : "error");
    if (n < 0 || n >= (int)sizeof(err)) {
        AIRY_STRNCPY_TERM(err, "{\"error\":{\"code\":500,\"message\":\"error\"}}", sizeof(err));
    }
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, status, err, strlen(err));
    int ret = MHD_NO;
    if (response) {
        ret = MHD_queue_response(connection, status, response);
        MHD_destroy_response(response);
    }
    atomic_fetch_add(&gateway->requests_failed, 1);
    return ret;
}

/**
  * @brief MHD content_reader: pull incremental chunks from the llm_d socket and wrap them as SSE events
 *
  * MHD semantics (microhttpd.h): a return >0 is the number of bytes written to buf;
  * MHD_CONTENT_READER_END_OF_STREAM (-1) marks the stream end (size=MHD_SIZE_UNKNOWN +
  * chunked encoding ends the final chunk). With size=MHD_SIZE_UNKNOWN,
  * pos is the accumulated output length (not relied upon here).
 *
  * Per round: `data: <chunk>\n\n` (chunks forwarded byte-wise).
  * On recv 0 (llm_d closed), timeout or error, emit `data: [DONE]\n\n`
  * set done; the next round returns MHD_CONTENT_READER_END_OF_STREAM.
 */
static ssize_t gw_sse_content_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)cls;
    if (!sctx || sctx->fd < 0 || sctx->done)
        return MHD_CONTENT_READER_END_OF_STREAM;
    /* Safety: buf must fit "data: "(6) + data + "\n\n"(2);
      * with block_size=1024, max>=1024, so this branch never triggers */
    if (max < sizeof(GW_SSE_DONE_EVENT))
        return MHD_CONTENT_READER_END_OF_STREAM;

    size_t want = max - 8;
    ssize_t n;
    do {
        n = recv(sctx->fd, buf + 6, want, 0);
    } while (n < 0 && errno == EINTR);
    if (n > 0) {
        AIRY_MEMCPY(buf, "data: ", 6);
        buf[6 + n] = '\n';
        buf[6 + n + 1] = '\n';
        return (ssize_t)(6 + n + 2);
    }

    sctx->done = 1;
    AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
    return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
}

/**
  * @brief MHD free_cb: release the SSE callback context (close fd + free memory)
 */
static void gw_sse_content_free(void *cls)
{
    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)cls;
    if (!sctx)
        return;
    if (sctx->fd >= 0)
        close(sctx->fd);
    AIRY_FREE(sctx);
}

/**
  * @brief Handle POST /api/v1/chat/stream (SSE streaming chat, CC=5)
 *
  * Request body (one of two):
  *   1. OpenAI format: {"model":"...","messages":[{"role":"user","content":"..."}]}
  *   2. Simplified JSON-RPC agent.run: {"jsonrpc":"2.0","method":"agent.run",
 *      "params":{"prompt":"...","model":"...","messages":[...]}}
  * messages may be empty; then build [{"role":"user","content":prompt}] from prompt;
  * Return 400 if both are missing.
 *
  * Flow: parse model/messages -> build the complete_stream JSON-RPC request ->
  * connect to llm_d (AIRY_LLM_SOCK -> $AIRY_RUNTIME_DIR/llm.sock, SO_RCVTIMEO 30s)
  * -> send -> stream via MHD_create_response_from_callback (content_reader
  * wraps each recv chunk as an SSE event, EOF emits [DONE]). 502 if llm_d is unreachable.
 */
int handle_chat_stream_sse(http_gateway_t *gateway, struct MHD_Connection *connection,
                           http_request_context_t *context)
{
#ifndef _WIN32
    const char *body = context->upload_data;
    size_t body_len = context->upload_data_size;
    if (!body || body_len == 0) {
        return gw_sse_send_json_error(gateway, connection, 400,
                                      "Request body required (model+messages or prompt)");
    }

    char *body_copy = (char *)AIRY_MALLOC(body_len + 1);
    if (!body_copy) {
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    AIRY_MEMCPY(body_copy, body, body_len);
    body_copy[body_len] = '\0';

    cJSON *root = cJSON_Parse(body_copy);
    AIRY_FREE(body_copy);
    if (!root) {
        return gw_sse_send_json_error(gateway, connection, 400, "Invalid JSON body");
    }

    const char *model = GW_SSE_DEFAULT_MODEL;
    const cJSON *messages = NULL;
    const cJSON *prompt = NULL;
    const cJSON *params = cJSON_GetObjectItem(root, "params");
    const cJSON *cfg = cJSON_IsObject(params) ? params : root;
    const cJSON *m = cJSON_GetObjectItem(cfg, "model");
    if (cJSON_IsString(m) && m->valuestring && m->valuestring[0])
        model = m->valuestring;
    const cJSON *pmsg = cJSON_GetObjectItem(cfg, "messages");
    if (cJSON_IsArray(pmsg))
        messages = pmsg;
    const cJSON *pp = cJSON_GetObjectItem(cfg, "prompt");
    if (cJSON_IsString(pp) && pp->valuestring && pp->valuestring[0])
        prompt = pp;

    cJSON *llm_params = cJSON_CreateObject();
    if (!llm_params) {
        cJSON_Delete(root);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    cJSON_AddStringToObject(llm_params, "model", model);
    if (messages && cJSON_GetArraySize(messages) > 0) {
        cJSON *dup = cJSON_Duplicate(messages, 1);
        if (!dup) {
            cJSON_Delete(llm_params);
            cJSON_Delete(root);
            return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
        }
        cJSON_AddItemToObject(llm_params, "messages", dup);
    } else if (prompt) {
        cJSON *arr = cJSON_CreateArray();
        cJSON *msg = cJSON_CreateObject();
        if (!arr || !msg) {
            if (msg)
                cJSON_Delete(msg);
            if (arr)
                cJSON_Delete(arr);
            cJSON_Delete(llm_params);
            cJSON_Delete(root);
            return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
        }
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", prompt->valuestring);
        cJSON_AddItemToArray(arr, msg);
        cJSON_AddItemToObject(llm_params, "messages", arr);
    } else {
        cJSON_Delete(llm_params);
        cJSON_Delete(root);
        return gw_sse_send_json_error(gateway, connection, 400, "messages or prompt required");
    }
    cJSON_Delete(root);

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        cJSON_Delete(llm_params);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", "complete_stream");
    cJSON_AddItemToObject(req, "params", llm_params);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        AIRY_FREE(req_str);
        return gw_sse_send_json_error(gateway, connection, 502, "LLM service unreachable");
    }
    char sock_path[256];
    gw_sse_resolve_llm_sock(sock_path, sizeof(sock_path));
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        AIRY_LOG_WARN("gateway sse: cannot connect to llm_d (sock=%s)", sock_path);
        close(fd);
        AIRY_FREE(req_str);
        return gw_sse_send_json_error(gateway, connection, 502, "LLM service unreachable");
    }

    struct timeval tv = {GW_SSE_RECV_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t len = strlen(req_str);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_str + sent, len - sent, 0);
        if (n <= 0) {
            AIRY_LOG_WARN("gateway sse: failed to send complete_stream to llm_d");
            close(fd);
            AIRY_FREE(req_str);
            return gw_sse_send_json_error(gateway, connection, 502, "LLM service unreachable");
        }
        sent += (size_t)n;
    }
    AIRY_FREE(req_str);

    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)AIRY_CALLOC(1, sizeof(gw_sse_ctx_t));
    if (!sctx) {
        close(fd);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    sctx->fd = fd;
    sctx->done = 0;

    struct MHD_Response *response =
        MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, GW_SSE_BLOCK_SIZE,
                                          gw_sse_content_reader, sctx, gw_sse_content_free);
    if (!response) {
        close(fd);
        AIRY_FREE(sctx);
        return gw_sse_send_json_error(gateway, connection, 500, "Failed to create stream response");
    }
    MHD_add_response_header(response, "Content-Type", "text/event-stream");
    MHD_add_response_header(response, "Cache-Control", "no-cache");
    MHD_add_response_header(response, "Connection", "keep-alive");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    gateway_apply_cors_headers(gateway, connection, response);

    atomic_fetch_add(&gateway->requests_total, 1);
    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    return ret;
#else
    (void)gateway;
    (void)connection;
    (void)context;
    return gw_sse_send_json_error(gateway, connection, 501,
                                  "SSE streaming unsupported on this platform");
#endif
}

/**
  * @brief HTTP route table (priority-ordered)
 *
  * Route matching rules:
  * 1. 1. Match the HTTP method
  * 2. 2. Match the path ("*" wildcard supported)
  * 3. 3. Fall back to the default route (handle_not_found)
 */
static const http_route_t http_routes[] = {{"POST", "/", handle_post_jsonrpc},
                                           {"POST", "/api/v1/chat/stream", handle_chat_stream_sse},
                                           {"OPTIONS", "*", handle_options_preflight},
                                           {"GET", "/health", handle_health_check},
                                           {"GET", "/metrics", handle_metrics_export},
                                           {NULL, NULL, handle_not_found}};

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
                                      .body = context->upload_data,
                                      .body_len = context->upload_data_size,
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

        MHD_get_connection_values(connection, MHD_HEADER_KIND, (MHD_KeyValueIterator)parse_headers,
                                  context);

        return MHD_YES;
    }

    if (strcmp(method, "POST") == 0 && upload_data && *upload_data_size > 0) {
        if (*upload_data_size > gateway->max_request_size) {
            return handle_request_too_large(gateway, connection, context, *upload_data_size);
        }

        context->upload_data = upload_data;
        context->upload_data_size = *upload_data_size;

        if (parse_json_request(gateway, context, upload_data, *upload_data_size) != 0) {
            return handle_parse_error(gateway, connection, context, *upload_data_size);
        }

        *upload_data_size = 0;
        return MHD_YES;
    }

    /* Stage 3: dispatch the complete request - both JSON-RPC and raw non-JSON-RPC bodies.
     * Except the SSE endpoint: its response is a continuous SSE event stream handled
     * directly by the static route (handle_chat_stream_sse), not a one-shot JSON reply. */
    if (strcmp(method, "POST") == 0 && strcmp(url, GW_SSE_CHAT_PATH) != 0 &&
        (context->json_request || (context->upload_data && context->upload_data_size > 0))) {
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
