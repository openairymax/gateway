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

/* OpenAI tools schema shared with gateway_d (SSoT, one-to-one with tool_d) */
#include "gateway_tools_schema.h"

/* Gateway-side hall event recording (write side of the SSoT event flow) */
#include "gateway_hall_store.h"

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

    const char *health_json =
        "{\"status\":\"healthy\",\"service\":\"gateway\",\"version\":\"0.1.2\"}";
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
  * The gateway proxies llm_d complete_stream and implements a full tool loop
  * (LLM -> tool_calls -> execute -> feed back -> continue, Claude-Code-style)
  * so plain daily chat can search the web / run commands / edit files.
  * Flow per request:
  *   1. Non-streaming llm.complete (with the full tool schema) judges whether
  *      the model wants tools.
  *   2. If it returns tool_calls: emit an SSE `tool_call` event, execute via
  *      tool_d execute_tool, emit an SSE `tool_result` event, feed the result
  *      back into messages, and repeat (capped at GW_SSE_MAX_TOOL_LOOPS).
  *   3. Once the model answers without tool_calls, stream that final text as
  *      SSE text chunks and terminate with `data: [DONE]`.
  *
  * SSE event envelope (TUI parses these):
  *   text:       data: <chunk>\n\n
  *   tool_call:  data: {"__airy_evt":"tool_call","tool":"...","args":{...}}\n\n
  *   tool_result:data: {"__airy_evt":"tool_result","tool":"...","ok":1,"summary":"..."}\n\n
  *   done:       data: [DONE]\n\n
  */
#define GW_SSE_CHAT_PATH "/api/v1/chat/stream"
#define GW_SSE_DEFAULT_MODEL "deepseek-v4-flash"
#define GW_SSE_RECV_TIMEOUT_S 90
#define GW_SSE_BLOCK_SIZE 1024
#define GW_SSE_DONE_EVENT "data: [DONE]\n\n"
#define GW_SSE_MAX_TOOL_LOOPS 8
#define GW_SSE_TEXT_CHUNK 512
#define GW_SSE_SUMMARY_MAX 256
/* Cap for tool results fed back to the LLM: web_fetch returns raw HTML that
 * can be tens/hundreds of KiB. Feeding it all burns tokens and drowns the
 * model; truncate with an explicit marker so the model knows content was cut
 * (bounds the complete request and keeps the tool loop cost sane). */
#define GW_SSE_TOOL_FEEDBACK_MAX 12288

/**
  * @brief SSE streaming response callback context
  *
  * cls for MHD_create_response_from_callback: holds the tool-loop state
  * machine. Freed by MHD's free_cb (gw_sse_content_free).
  *
  * The content_reader is a pull model: each MHD callback invocation produces
  * exactly one SSE frame. Tool execution (tool_d execute_tool) blocks inside
  * the callback like the plain llm_d recv does today; SSE is a long-lived
  * connection so a blocking step is acceptable.
  */
typedef enum {
    GW_SSE_PHASE_LLM_ROUND = 0, /* waiting for / parsing a non-streaming LLM reply */
    GW_SSE_PHASE_EXEC_TOOLS,    /* executing pending tool_calls one by one */
    GW_SSE_PHASE_FINAL_TEXT,    /* chunking the final reply text */
    GW_SSE_PHASE_DONE           /* [DONE] emitted */
} gw_sse_phase_t;

typedef struct {
    /* sockets (reconnected per step; only one live at a time) */
    char llm_sock[256];
    char tool_sock[256];
    int fd;           /* current llm.sock fd, -1 when idle */
    int done;
    int phase;
    /* request context */
    char *model;
    cJSON *messages;  /* conversation history (with tool feedback) */
    int tool_round;
    /* pending tool calls from the current LLM round */
    cJSON *tool_calls;
    int tc_count;
    int tc_idx;
    int exec_done;    /* 1 = current tool already executed (result stashed) */
    char *stash_result; /* tool result text for the pending tool_result event */
    /* final text streaming */
    char *final_text;
    size_t final_len;
    size_t final_pos;
    /* in-flight step result (one SSE frame), written by the current phase */
    char *step_buf;
    size_t step_len;
    /* hall event recording (gateway_hall_store): task ID for this session
     * (generated when no client session_id is provided) + dedup flags */
    char task_id[64];
    int recorded_result;
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
  * @brief Resolve the tool_d socket path: env AIRY_TOOL_SOCK -> airy_runtime_dir()/tool.sock
 */
static void gw_sse_resolve_tool_sock(char *out, size_t out_size)
{
    const char *env = getenv("AIRY_TOOL_SOCK");
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_size);
        return;
    }
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir) {
        snprintf(out, out_size, "%s/tool.sock", run_dir);
    } else {
        AIRY_STRNCPY_TERM(out, "tool.sock", out_size);
    }
}

/* Record one hall event for an SSE session (best effort; a failed event
 * write must never disturb the stream). `content` must be a JSON object. */
static void gw_sse_record_event(gw_sse_ctx_t *sctx, const char *category, cJSON *content)
{
    if (!sctx || !sctx->task_id[0] || !category || !content)
        return;
    char *content_str = cJSON_PrintUnformatted(content);
    if (!content_str)
        return;
    (void)gw_hall_store_event(sctx->task_id, category, NULL, content_str);
    AIRY_FREE(content_str);
}

/* Last user message content from the conversation history (SSE sessions may
 * arrive as OpenAI-format messages without an explicit prompt field). */
static void gw_sse_user_prompt(const cJSON *messages, char *out, size_t out_sz)
{
    out[0] = '\0';
    if (!cJSON_IsArray(messages))
        return;
    int n = cJSON_GetArraySize(messages);
    for (int i = n - 1; i >= 0; i--) {
        cJSON *m = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (cJSON_IsString(role) && role->valuestring && strcmp(role->valuestring, "user") == 0) {
            cJSON *c = cJSON_GetObjectItem(m, "content");
            if (cJSON_IsString(c) && c->valuestring) {
                AIRY_STRNCPY_TERM(out, c->valuestring, out_sz);
                return;
            }
        }
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
  * @brief Connect a Unix socket, send a JSON-RPC request and read the full response
  *
  * Used for the non-streaming llm.complete and tool.execute_tool round trips
  * inside the SSE tool loop. Returns the raw JSON-RPC response string
  * (AIRY_MALLOC) or NULL on failure.
 */
static char *gw_sse_rpc(const char *sock_path, const char *req_json, int timeout_s)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    struct timeval tv = {timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t len = strlen(req_json);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_json + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        close(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > 1048576) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    close(fd);
    return resp;
#else
    (void)sock_path;
    (void)req_json;
    (void)timeout_s;
    return NULL;
#endif
}

/**
  * @brief Build a non-streaming llm.complete JSON-RPC request (with the tool schema)
 */
static char *gw_sse_build_llm_request(const char *model, const cJSON *messages)
{
    cJSON *llm_req = cJSON_CreateObject();
    if (!llm_req)
        return NULL;
    cJSON_AddStringToObject(llm_req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(llm_req, "id", 1);
    cJSON_AddStringToObject(llm_req, "method", "complete");
    cJSON *llm_params = cJSON_CreateObject();
    if (!llm_params) {
        cJSON_Delete(llm_req);
        return NULL;
    }
    cJSON_AddStringToObject(llm_params, "model", model);
    cJSON_AddItemToObject(llm_params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = cJSON_Parse(GW_TOOLS_JSON_SOURCE);
    if (tools) {
        cJSON_AddItemToObject(llm_params, "tools", tools);
    }
    cJSON_AddNumberToObject(llm_params, "max_tokens", 2048);
    cJSON_AddNumberToObject(llm_params, "temperature", 0.7);
    cJSON_AddItemToObject(llm_req, "params", llm_params);
    char *req_str = cJSON_PrintUnformatted(llm_req);
    cJSON_Delete(llm_req);
    return req_str;
}

/**
  * @brief Extract tool_calls from an llm_d complete response
  * @return 0 with *out (caller cJSON_Delete) present; non-zero without
  */
static int gw_sse_parse_tool_calls(const char *llm_resp, cJSON **out)
{
    *out = NULL;
    cJSON *root = cJSON_Parse(llm_resp);
    if (!root)
        return -1;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *tc = choice0 ? cJSON_GetObjectItem(choice0, "tool_calls") : NULL;
    if (cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0) {
        *out = cJSON_Duplicate(tc, 1);
    }
    cJSON_Delete(root);
    return *out ? 0 : -1;
}

/**
  * @brief Extract the reply text (choices[0].content) from an llm_d complete response
  * @return AIRY_MALLOC text ("" when absent); NULL only on parse failure
 */
static char *gw_sse_parse_content(const char *llm_resp)
{
    cJSON *root = cJSON_Parse(llm_resp);
    if (!root)
        return NULL;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *content = choice0 ? cJSON_GetObjectItem(choice0, "content") : NULL;
    char *text = AIRY_STRDUP(cJSON_IsString(content) && content->valuestring ? content->valuestring
                                                                             : "");
    cJSON_Delete(root);
    return text;
}

/**
  * @brief Execute one tool via tool_d execute_tool
  * @return 0 on success (*out_text AIRY_MALLOC result text), non-zero on failure
  */
static int gw_sse_execute_tool(const char *tool_sock, const char *name, const char *args_json,
                               char **out_text)
{
    *out_text = NULL;
    cJSON *req = cJSON_CreateObject();
    if (!req)
        return -1;
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", "execute_tool");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    cJSON_AddItemToObject(req, "params", params);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str)
        return -1;

    char *resp = gw_sse_rpc(tool_sock, req_str, GW_SSE_RECV_TIMEOUT_S);
    AIRY_FREE(req_str);
    if (!resp) {
        *out_text = AIRY_STRDUP("Tool service unreachable");
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *out_text = AIRY_STRDUP("Tool service returned invalid response");
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *err = cJSON_GetObjectItem(root, "error");
    int tool_ok = 0;
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        tool_ok = cJSON_IsNumber(success) && success->valueint != 0;
        if (tool_ok) {
            *out_text = AIRY_STRDUP(cJSON_IsString(output) && output->valuestring
                                        ? output->valuestring
                                        : "(no output)");
        } else {
            const char *e = cJSON_IsString(error) && error->valuestring ? error->valuestring
                                                                         : "execution failed";
            size_t elen = strlen(e) + 8;
            *out_text = (char *)AIRY_MALLOC(elen);
            if (*out_text)
                snprintf(*out_text, elen, "Error: %s", e);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        const char *m = cJSON_IsString(msg) && msg->valuestring ? msg->valuestring : "RPC error";
        size_t elen = strlen(m) + 8;
        *out_text = (char *)AIRY_MALLOC(elen);
        if (*out_text)
            snprintf(*out_text, elen, "Error: %s", m);
    } else {
        *out_text = AIRY_STRDUP("Tool service returned no result");
    }
    cJSON_Delete(root);
    return tool_ok ? 0 : -1;
}

/**
  * @brief Build an SSE frame "data: <payload>\n\n" (AIRY_MALLOC)
 */
static char *gw_sse_frame(const char *payload)
{
    size_t plen = strlen(payload);
    /* "data: " (6) + payload + "\n\n" (2) + NUL (1) = plen + 9 */
    char *frame = (char *)AIRY_MALLOC(plen + 9);
    if (!frame)
        return NULL;
    AIRY_MEMCPY(frame, "data: ", 6);
    AIRY_MEMCPY(frame + 6, payload, plen);
    frame[6 + plen] = '\n';
    frame[6 + plen + 1] = '\n';
    frame[6 + plen + 2] = '\0';
    return frame;
}

/**
  * @brief Append a tool result message to the conversation history
  */
static void gw_sse_append_tool_result(cJSON *messages, const char *tool_call_id,
                                      const char *content)
{
    cJSON *tool_msg = cJSON_CreateObject();
    if (!tool_msg)
        return;
    cJSON_AddStringToObject(tool_msg, "role", "tool");
    cJSON_AddStringToObject(tool_msg, "tool_call_id", tool_call_id);
    cJSON_AddStringToObject(tool_msg, "content", content ? content : "Tool execution failed");
    cJSON_AddItemToArray(messages, tool_msg);
}

/**
  * @brief Truncate a long result to a client-facing summary (AIRY_MALLOC)
  */
static char *gw_sse_summary(const char *text)
{
    if (!text)
        return AIRY_STRDUP("");
    size_t len = strlen(text);
    if (len <= GW_SSE_SUMMARY_MAX)
        return AIRY_STRDUP(text);
    char *sum = (char *)AIRY_MALLOC(GW_SSE_SUMMARY_MAX + 16);
    if (!sum)
        return AIRY_STRDUP("");
    AIRY_MEMCPY(sum, text, GW_SSE_SUMMARY_MAX);
    snprintf(sum + GW_SSE_SUMMARY_MAX, 16, "... (%zu bytes)", len);
    return sum;
}

/**
 * @brief Tool result text fed back to the LLM as a role="tool" message
 *
 * Truncates oversized results (raw HTML from web_fetch) to
 * GW_SSE_TOOL_FEEDBACK_MAX bytes with an explicit "[truncated: N bytes]"
 * marker, so the model knows the content was cut. Returns AIRY_MALLOC.
 */
static char *gw_sse_feedback(const char *text)
{
    if (!text)
        return AIRY_STRDUP("Tool execution failed");
    size_t len = strlen(text);
    if (len <= GW_SSE_TOOL_FEEDBACK_MAX)
        return AIRY_STRDUP(text);
    char *fb = (char *)AIRY_MALLOC(GW_SSE_TOOL_FEEDBACK_MAX + 48);
    if (!fb)
        return AIRY_STRDUP(text);
    AIRY_MEMCPY(fb, text, GW_SSE_TOOL_FEEDBACK_MAX);
    snprintf(fb + GW_SSE_TOOL_FEEDBACK_MAX, 48, "\n...[truncated: %zu bytes]", len);
    return fb;
}

/**
  * @brief MHD content_reader: drive the SSE tool-loop state machine
  *
  * MHD semantics (microhttpd.h): a return >0 is the number of bytes written
  * to buf; MHD_CONTENT_READER_END_OF_STREAM (-1) marks the stream end.
  * Each invocation produces exactly one SSE frame (the step_buf produced by
  * the current phase); phases are advanced step by step:
  *   LLM_ROUND   -> non-streaming llm.complete, parse tool_calls; if the
  *                  model calls tools, emit the first tool_call event and
  *                  switch to EXEC_TOOLS; otherwise chunk the reply text.
  *   EXEC_TOOLS  -> emit tool_call event (first invocation per tool), then
  *                  execute via tool.sock, feed back and emit tool_result;
  *                  loop through all pending calls, then back to LLM_ROUND.
  *   FINAL_TEXT  -> emit text chunks; on exhaustion emit [DONE], phase DONE.
  */
static ssize_t gw_sse_content_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)cls;
    if (!sctx || sctx->done)
        return MHD_CONTENT_READER_END_OF_STREAM;
    if (max < 8)
        return MHD_CONTENT_READER_END_OF_STREAM;

    /* Drain the current step frame first (may span several callbacks). */
    if (sctx->step_buf && sctx->step_len > 0) {
        size_t n = sctx->step_len < max ? sctx->step_len : max;
        AIRY_MEMCPY(buf, sctx->step_buf, n);
        size_t rem = sctx->step_len - n;
        if (rem > 0) {
            AIRY_MEMCPY(sctx->step_buf, sctx->step_buf + n, rem);
            sctx->step_len = rem;
        } else {
            AIRY_FREE(sctx->step_buf);
            sctx->step_buf = NULL;
            sctx->step_len = 0;
        }
        return (ssize_t)n;
    }

    for (;;) {
        switch (sctx->phase) {
        case GW_SSE_PHASE_LLM_ROUND: {
            if (sctx->tool_round >= GW_SSE_MAX_TOOL_LOOPS) {
                /* Too many rounds: emit what we have as the final answer. */
                sctx->final_text = AIRY_STRDUP("(tool loop limit reached)");
                sctx->final_len = strlen(sctx->final_text);
                sctx->final_pos = 0;
                sctx->phase = GW_SSE_PHASE_FINAL_TEXT;
                continue;
            }
            char *req_str = gw_sse_build_llm_request(sctx->model, sctx->messages);
            if (!req_str) {
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }
            char *llm_resp = gw_sse_rpc(sctx->llm_sock, req_str, GW_SSE_RECV_TIMEOUT_S);
            AIRY_FREE(req_str);
            if (!llm_resp) {
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }

            cJSON *tool_calls = NULL;
            gw_sse_parse_tool_calls(llm_resp, &tool_calls);
            char *text = gw_sse_parse_content(llm_resp);

            /* Preserve the assistant round in history (for the next LLM call). */
            if (tool_calls) {
                cJSON *assistant_msg = cJSON_CreateObject();
                if (assistant_msg) {
                    cJSON_AddStringToObject(assistant_msg, "role", "assistant");
                    cJSON_AddStringToObject(assistant_msg, "content", text ? text : "");
                    cJSON_AddItemToObject(assistant_msg, "tool_calls",
                                          cJSON_Duplicate(tool_calls, 1));
                    cJSON_AddItemToArray(sctx->messages, assistant_msg);
                }
                sctx->tool_calls = tool_calls;
                sctx->tc_count = cJSON_GetArraySize(tool_calls);
                sctx->tc_idx = 0;
                sctx->phase = GW_SSE_PHASE_EXEC_TOOLS;
                sctx->tool_round++;
                if (text)
                    AIRY_FREE(text);
                continue; /* fall through to EXEC_TOOLS below */
            }

            /* Final answer without tools: chunk it. */
            sctx->final_text = text ? text : AIRY_STRDUP("");
            sctx->final_len = strlen(sctx->final_text);
            sctx->final_pos = 0;
            sctx->phase = GW_SSE_PHASE_FINAL_TEXT;
            AIRY_FREE(llm_resp);
            continue;
        }

        case GW_SSE_PHASE_EXEC_TOOLS: {
            if (!sctx->tool_calls || sctx->tc_idx >= sctx->tc_count) {
                /* All pending tools done: next LLM round. */
                if (sctx->tool_calls) {
                    cJSON_Delete(sctx->tool_calls);
                    sctx->tool_calls = NULL;
                }
                if (sctx->stash_result) {
                    AIRY_FREE(sctx->stash_result);
                    sctx->stash_result = NULL;
                }
                sctx->exec_done = 0;
                sctx->phase = GW_SSE_PHASE_LLM_ROUND;
                continue;
            }

            cJSON *tc = cJSON_GetArrayItem(sctx->tool_calls, sctx->tc_idx);
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *fn_name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *fn_args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            const char *tname = cJSON_IsString(fn_name) ? fn_name->valuestring : "";
            const char *targs = cJSON_IsString(fn_args) ? fn_args->valuestring : "{}";
            const char *tid = cJSON_IsString(tc_id) ? tc_id->valuestring : "";

            if (!sctx->exec_done) {
                /* First pass: emit the tool_call event, then execute
                 * immediately (blocking) and stash the result so the next
                 * callback can emit the tool_result frame. */
                cJSON *evt = cJSON_CreateObject();
                if (evt) {
                    cJSON_AddStringToObject(evt, "__airy_evt", "tool_call");
                    cJSON_AddStringToObject(evt, "tool", tname);
                    cJSON *pargs = cJSON_Parse(targs);
                    if (pargs)
                        cJSON_AddItemToObject(evt, "args", pargs);
                    else
                        cJSON_AddStringToObject(evt, "args", targs);
                    char *evt_str = cJSON_PrintUnformatted(evt);
                    cJSON_Delete(evt);
                    sctx->step_buf = evt_str ? gw_sse_frame(evt_str) : gw_sse_frame("{}");
                    if (evt_str)
                        AIRY_FREE(evt_str);
                } else {
                    sctx->step_buf = gw_sse_frame("{}");
                }
                if (sctx->step_buf)
                    sctx->step_len = strlen(sctx->step_buf);

                char *result_text = NULL;
                gw_sse_execute_tool(sctx->tool_sock, tname, targs, &result_text);
                if (sctx->stash_result)
                    AIRY_FREE(sctx->stash_result);
                sctx->stash_result = result_text;
                sctx->exec_done = 1;
                break; /* return the tool_call frame */
            }

            /* Second pass: emit the tool_result frame and feed back. */
            const char *res = sctx->stash_result ? sctx->stash_result : "Tool execution failed";
            int tool_ok = sctx->stash_result && res[0] != '\0' ? 1 : 0;
            char *summary = gw_sse_summary(res);
            char *feedback = gw_sse_feedback(res);
            cJSON *evt2 = cJSON_CreateObject();
            if (evt2) {
                cJSON_AddStringToObject(evt2, "__airy_evt", "tool_result");
                cJSON_AddStringToObject(evt2, "tool", tname);
                cJSON_AddStringToObject(evt2, "call_id", tid);
                cJSON_AddNumberToObject(evt2, "ok", tool_ok);
                cJSON_AddStringToObject(evt2, "summary", summary ? summary : "");
                char *evt_str = cJSON_PrintUnformatted(evt2);
                cJSON_Delete(evt2);
                sctx->step_buf = evt_str ? gw_sse_frame(evt_str) : gw_sse_frame("{}");
                if (evt_str)
                    AIRY_FREE(evt_str);
            } else {
                sctx->step_buf = gw_sse_frame("{}");
            }
            if (sctx->step_buf)
                sctx->step_len = strlen(sctx->step_buf);

            gw_sse_append_tool_result(sctx->messages, tid,
                                      feedback ? feedback : res);
            AIRY_FREE(feedback);
            AIRY_FREE(summary);
            if (sctx->stash_result) {
                AIRY_FREE(sctx->stash_result);
                sctx->stash_result = NULL;
            }
            sctx->exec_done = 0;
            sctx->tc_idx++;
            /* Record one tool-call event per executed tool (deduped: this is
             * the second pass, i.e. exactly once per tool). tool_ok was
             * evaluated above before stash_result is freed. */
            cJSON *tevt = cJSON_CreateObject();
            if (tevt) {
                cJSON_AddStringToObject(tevt, "event", "tool_call");
                cJSON_AddStringToObject(tevt, "tool", tname);
                cJSON_AddNumberToObject(tevt, "ok", tool_ok);
                gw_sse_record_event(sctx, "chain", tevt);
                cJSON_Delete(tevt);
            }
            break; /* return the tool_result frame */
        }

        case GW_SSE_PHASE_FINAL_TEXT: {
            if (sctx->final_pos >= sctx->final_len) {
                /* Record the completion into the hall event flow exactly
                 * once (this branch runs a single time before [DONE]). */
                if (!sctx->recorded_result) {
                    sctx->recorded_result = 1;
                    cJSON *revt = cJSON_CreateObject();
                    if (revt) {
                        cJSON_AddStringToObject(revt, "event", "chat_result");
                        cJSON_AddNumberToObject(revt, "ok", 1);
                        char tbuf[520];
                        AIRY_STRNCPY_TERM(tbuf, sctx->final_text ? sctx->final_text : "",
                                          sizeof(tbuf));
                        cJSON_AddStringToObject(revt, "text", tbuf);
                        gw_sse_record_event(sctx, "result", revt);
                        cJSON_Delete(revt);
                    }
                }
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }
            size_t n = sctx->final_len - sctx->final_pos;
            if (n > GW_SSE_TEXT_CHUNK)
                n = GW_SSE_TEXT_CHUNK;
            sctx->step_buf = (char *)AIRY_MALLOC(n + 9);
            if (!sctx->step_buf) {
                sctx->done = 1;
                AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
                return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
            }
            AIRY_MEMCPY(sctx->step_buf, "data: ", 6);
            AIRY_MEMCPY(sctx->step_buf + 6, sctx->final_text + sctx->final_pos, n);
            sctx->step_buf[6 + n] = '\n';
            sctx->step_buf[6 + n + 1] = '\n';
            sctx->step_buf[6 + n + 2] = '\0';
            sctx->step_len = 6 + n + 2;
            sctx->final_pos += n;
            break;
        }

        default:
            sctx->done = 1;
            AIRY_MEMCPY(buf, GW_SSE_DONE_EVENT, sizeof(GW_SSE_DONE_EVENT) - 1);
            return (ssize_t)(sizeof(GW_SSE_DONE_EVENT) - 1);
        }

        if (sctx->step_buf && sctx->step_len > 0) {
            size_t n = sctx->step_len < max ? sctx->step_len : max;
            AIRY_MEMCPY(buf, sctx->step_buf, n);
            size_t rem = sctx->step_len - n;
            if (rem > 0) {
                AIRY_MEMCPY(sctx->step_buf, sctx->step_buf + n, rem);
                sctx->step_len = rem;
            } else {
                AIRY_FREE(sctx->step_buf);
                sctx->step_buf = NULL;
                sctx->step_len = 0;
            }
            return (ssize_t)n;
        }
    }
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
    AIRY_FREE((void *)sctx->model);
    if (sctx->messages)
        cJSON_Delete(sctx->messages);
    if (sctx->tool_calls)
        cJSON_Delete(sctx->tool_calls);
    if (sctx->stash_result)
        AIRY_FREE(sctx->stash_result);
    AIRY_FREE(sctx->final_text);
    AIRY_FREE(sctx->step_buf);
    AIRY_FREE(sctx);
}

/**
  * @brief Handle POST /api/v1/chat/stream (SSE streaming chat with tool loop)
  *
  * Request body (one of two):
  *   1. OpenAI format: {"model":"...","messages":[{"role":"user","content":"..."}]}
  *   2. Simplified JSON-RPC agent.run: {"jsonrpc":"2.0","method":"agent.run",
  *      "params":{"prompt":"...","model":"...","messages":[...]}}
  * messages may be empty; then build [{"role":"user","content":prompt}] from prompt;
  * Return 400 if both are missing.
  *
  * Flow: parse model/messages -> seed the tool-loop state machine (messages
  * kept in cJSON form) -> stream via MHD_create_response_from_callback.
  * The content_reader drives non-streaming llm.complete rounds (with the full
  * tool schema), executes tool_calls through tool.sock, feeds results back,
  * and finally chunks the reply text as SSE events terminated by [DONE].
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

    /* Seed the tool-loop conversation history: duplicate messages, or build a
     * single user message from prompt. */
    cJSON *history = NULL;
    if (messages && cJSON_GetArraySize(messages) > 0) {
        history = cJSON_Duplicate(messages, 1);
    } else if (prompt) {
        history = cJSON_CreateArray();
        if (history) {
            cJSON *msg = cJSON_CreateObject();
            if (!msg) {
                cJSON_Delete(history);
                history = NULL;
            } else {
                cJSON_AddStringToObject(msg, "role", "user");
                cJSON_AddStringToObject(msg, "content", prompt->valuestring);
                cJSON_AddItemToArray(history, msg);
            }
        }
    }
    /* model may point into `root`; duplicate it before root is freed so the
     * session model string never dangles after cJSON_Delete(root). */
    char *model_dup = AIRY_STRDUP(model);
    cJSON_Delete(root);
    if (!model_dup || !history) {
        AIRY_FREE(model_dup);
        return gw_sse_send_json_error(gateway, connection, 400, "messages or prompt required");
    }

    gw_sse_ctx_t *sctx = (gw_sse_ctx_t *)AIRY_CALLOC(1, sizeof(gw_sse_ctx_t));
    if (!sctx) {
        cJSON_Delete(history);
        AIRY_FREE(model_dup);
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    }
    sctx->fd = -1;
    sctx->done = 0;
    sctx->phase = GW_SSE_PHASE_LLM_ROUND;
    sctx->model = model_dup;
    sctx->messages = history;
    sctx->tool_round = 0;
    gw_sse_resolve_llm_sock(sctx->llm_sock, sizeof(sctx->llm_sock));
    gw_sse_resolve_tool_sock(sctx->tool_sock, sizeof(sctx->tool_sock));

    /* Record the session start into the hall event flow (SSoT write side):
     * every gateway chat session becomes visible on the board / decision
     * chain / event stream, keeping the "seen == recorded" invariant. */
    gw_hall_task_id_now(sctx->task_id, sizeof(sctx->task_id));
    cJSON *start_evt = cJSON_CreateObject();
    if (start_evt) {
        cJSON_AddStringToObject(start_evt, "event", "chat_start");
        char pbuf[520];
        gw_sse_user_prompt(history, pbuf, sizeof(pbuf));
        cJSON_AddStringToObject(start_evt, "prompt", pbuf);
        gw_sse_record_event(sctx, "chain", start_evt);
        cJSON_Delete(start_evt);
    }

    struct MHD_Response *response =
        MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, GW_SSE_BLOCK_SIZE,
                                          gw_sse_content_reader, sctx, gw_sse_content_free);
    if (!response) {
        gw_sse_content_free(sctx);
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
