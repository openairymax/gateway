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
#include "http_gateway_deps.h"

#include "gateway_auth.h"

/* OpenAI tools schema shared with gateway_d (SSoT, one-to-one with tool_d) */
#include "airy_tool_schema.h"

/* Gateway-side hall event recording (write side of the SSoT event flow) */
#include "gateway_hall_store.h"

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
                             http_request_context_t *context)
{

    struct MHD_Response *response =
        MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);

    gateway_apply_security_headers(response);
    gateway_apply_cors_headers(gateway, connection, response);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);

    return ret;
}

/* ================= 入口鉴权 =================
 * 纯判定逻辑在 gateway_auth.c（可独立单测），此处仅做 MHD 桥接（K-1）：
 * 提取凭证与对端地址 → 调用策略 → 拒绝时经统一工厂出 401。 */

/**
 * @brief 提取真实 socket 对端地址并判定回环（T-11a）
 *
 * 安全输入只认 socket 地址：X-Forwarded-For 可伪造，仅限流可用，
 * 不得作为鉴权输入。
 */
static int gateway_peer_is_loopback(struct MHD_Connection *connection)
{
    const union MHD_ConnectionInfo *cinfo =
        MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    const struct sockaddr *addr = cinfo ? (const struct sockaddr *)cinfo->client_addr : NULL;
    if (!addr)
        return 0; /* 地址不可得：fail-closed 视为非回环 */

    char addr_buf[64];
    if (addr->sa_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)addr)->sin_addr, addr_buf,
                  sizeof(addr_buf));
    } else if (addr->sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)addr)->sin6_addr, addr_buf,
                  sizeof(addr_buf));
    } else {
        return 0;
    }
    return gw_auth_addr_is_loopback(addr_buf);
}

/* 轻量 JSON 拒绝回包：security + CORS 头必加；server 为 NULL 时不写
 * Server 头（非法路径拒绝的既有形态）。 */
static int gw_deny_json(http_gateway_t *gateway, struct MHD_Connection *connection, int status,
                        const char *body, const char *server)
{
    struct MHD_Response *response =
        MHD_create_response_from_buffer(strlen(body), (void *)body, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(response, "Content-Type", "application/json");
    if (server)
        MHD_add_response_header(response, "Server", server);
    gateway_apply_security_headers(response);
    gateway_apply_cors_headers(gateway, connection, response);
    int ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

/**
 * @brief 统一 401 拒绝响应（T-11a：CORS-safe 工厂 + 安全头）
 */
static int queue_unauth(http_gateway_t *gateway,
                                           struct MHD_Connection *connection)
{
    const char *err_json =
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"Unauthorized: "
        "API key required\"},\"id\":null}";
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 401, err_json, strlen(err_json));
    atomic_fetch_add(&gateway->requests_failed, 1);
    int ret = MHD_queue_response(connection, 401, response);
    MHD_destroy_response(response);
    return ret;
}

/**
 * @brief 入口鉴权门禁（T-11a）：敏感路由统一强制
 *
 * 判定语义（fail-closed 双层）见 gateway_auth.c：key 已配置 → 凭证必查
 * （回环不豁免）；未配置 → 仅放行回环对端。
 *
 * 返回值必须与 MHD 回调值解耦：MHD_queue_response 成功时同样返回
 * MHD_YES，若直接以回调值判断放行，拒绝分支会被误判为通过，未授权
 * 请求会继续累积 body 并进入 JSON-RPC 执行。
 * @return 0=放行（继续正常处理）；1=已回包拒绝，*ret 为回调返回值
 */
static int gateway_enforce_entry_auth(http_gateway_t *gateway, struct MHD_Connection *connection,
                                      int *ret)
{
    const char *auth_header =
        MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
    const char *key_param =
        MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "api_key");

    int verdict = gw_auth_decide(1, gw_auth_key_matches(auth_header, key_param),
                                 gw_auth_key_configured(), gateway_peer_is_loopback(connection));
    if (verdict == GW_AUTH_DENY) {
        AIRY_LOG_WARN("gateway auth: request denied (sensitive route, no valid credential)");
        *ret = queue_unauth(gateway, connection);
        return 1;
    }
    return 0;
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
                        http_request_context_t *context)
{

    char health_json[192];
    int n = snprintf(health_json, sizeof(health_json),
                     "{\"status\":\"healthy\",\"service\":\"gateway\",\"version\":\""
                     GATEWAY_VERSION "\",\"epoch\":%llu}",
                     (unsigned long long)gateway->epoch_s);
    if (n <= 0 || (size_t)n >= sizeof(health_json)) {
        return MHD_NO;
    }

    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 200, health_json, (size_t)n);

    atomic_fetch_add(&gateway->requests_total, 1);

    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);

    return ret;
}

/**
  * @brief Handle GET /metrics (CC=2)
  *
  * T-11a 后鉴权由入口门禁统一强制（路由表 auth_required=1），本处理器
  * 不再自校验（删除旧 strcmp 版 gateway_verify_api_key，T-17 恒定时间
  * 比较由 gw_auth_key_matches 兼收）。
 */
int handle_metrics_export(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context)
{
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
                             http_request_context_t *context,
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
                       http_request_context_t *context, size_t data_size)
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
  * auth_required：1=敏感面（入口鉴权门禁强制），
  * 0=公开面（OPTIONS 预检、/health 健康探针）或 404 兜底行。
  *
  * Route matching rules:
  * 1. 1. Match the HTTP method
  * 2. 2. Match the path ("*" wildcard supported)
  * 3. 3. Fall back to the default route (handle_not_found)
 */
static const http_route_t http_routes[] = {{"POST", "/", handle_post_jsonrpc, 0, 1},
                                           {"POST", GW_SSE_RUN_STREAM_PATH, handle_run_stream_sse, 1, 1},
                                           {"GET", "/api/v1/hall/watch", handle_hall_watch_sse, 0, 1},
                                           {"OPTIONS", "*", handle_options_preflight, 0, 0},
                                           {"GET", "/health", handle_health_check, 0, 0},
                                           {"GET", "/metrics", handle_metrics_export, 0, 1},
                                           {NULL, NULL, handle_not_found, 0, 0}};

/**
  * @brief 路由敏感性分类器（T-11a，SSoT：http_routes 表 auth_required 字段）
  *
  * NULL 方法/URL 与未登记路由一律按公开面（0）处理——真实请求随后由
  * find_http_route 走 404 兜底，无敏感数据可泄露。
 */
int http_gateway_route_auth_required(const char *method, const char *url)
{
    if (!method || !url)
        return 0;
    for (const http_route_t *route = http_routes; route->method != NULL; route++) {
        if (strcmp(method, route->method) == 0 &&
            (strcmp(route->path, "*") == 0 || strcmp(url, route->path) == 0)) {
            return route->auth_required;
        }
    }
    return 0;
}

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

/* 限流键：X-Forwarded-For → X-Real-IP → socket 对端地址；全不可得时
 * 归并到 _unresolved（同桶限流，不因取值失败而放行）。 */
static const char *gw_client_ip(struct MHD_Connection *connection, char *buf, size_t sz)
{
    const char *ip = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Forwarded-For");
    if (!ip)
        ip = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Real-IP");
    if (ip)
        return ip;

    const union MHD_ConnectionInfo *cinfo =
        MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    const struct sockaddr *addr = cinfo ? (const struct sockaddr *)cinfo->client_addr : NULL;
    if (!addr)
        return "_unresolved";
    if (addr->sa_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)addr)->sin_addr, buf, sz);
    } else if (addr->sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)addr)->sin6_addr, buf, sz);
    } else {
        return "_unresolved";
    }
    return buf;
}

/* 限流门禁：超限直接回 429；0=放行，1=已回包（*ret 为回调返回值） */
static int gw_rate_gate(http_gateway_t *gateway, struct MHD_Connection *connection, int *ret)
{
    if (!gateway->rate_limiter)
        return 0;

    char ip_buf[64];
    const char *client_ip = gw_client_ip(connection, ip_buf, sizeof(ip_buf));
    if (gateway_rate_limiter_allow(gateway->rate_limiter, client_ip))
        return 0;

    *ret = gw_deny_json(gateway, connection, 429,
                        "{\"error\":{\"code\":-32004,\"message\":\"Rate limit exceeded\"}}",
                        "AgentRT-gateway/1.0");
    return 1;
}

/* 入口鉴权门禁：仅敏感面生效（限流后、上下文分配前）；0=放行，1=已回包 */
static int gw_auth_gate(http_gateway_t *gateway, struct MHD_Connection *connection,
                        const char *url, const char *method, int *ret)
{
    if (!http_gateway_route_auth_required(method, url))
        return 0;
    return gateway_enforce_entry_auth(gateway, connection, ret);
}

/* 首次回调：分配并登记请求上下文；非法路径在 body 累积前拒绝 */
static int gw_ctx_new(http_gateway_t *gateway, struct MHD_Connection *connection, const char *url,
                      const char *method, void **con_cls)
{
    http_request_context_t *context = AIRY_CALLOC(1, sizeof(http_request_context_t));
    if (!context)
        return MHD_NO;

    if (!gateway_is_url_safe(url)) {
        AIRY_FREE(context);
        return gw_deny_json(gateway, connection, 400,
                            "{\"error\":{\"code\":-32002,\"message\":\"Invalid URL path\"}}", NULL);
    }

    context->method = method;
    context->url = url;
    context->start_time_ns = gateway_time_ns();
    *con_cls = context;
    return MHD_YES;
}

/* POST body 累积：MHD 分块投递并在块间复用 upload_data 缓冲，必须
 * 自持累积缓冲（只记末块指针会截断 body）。 */
static int gw_body_append(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context, const char *data, size_t *len)
{
    size_t total = context->body_len + *len;
    if (total > gateway->max_request_size)
        return handle_request_too_large(gateway, connection, context, total);

    if (total > context->body_cap) {
        size_t new_cap = context->body_cap == 0 ? 4096 : context->body_cap;
        while (new_cap < total)
            new_cap *= 2;
        char *nb = AIRY_REALLOC(context->body_buf, new_cap);
        if (!nb)
            return MHD_NO;
        context->body_buf = nb;
        context->body_cap = new_cap;
    }

    AIRY_MEMCPY(context->body_buf + context->body_len, data, *len);
    context->body_len = total;
    *len = 0;
    return MHD_YES;
}

/* 完成态分发：JSON-RPC 聚合（流式端点除外，SSoT 见路由表）→ 路由表
 * → 404 兜底。 */
static int gw_dispatch(http_gateway_t *gateway, struct MHD_Connection *connection,
                       http_request_context_t *context, const char *url, const char *method)
{
    if (strcmp(method, "POST") == 0 && !is_streaming_route(method, url) &&
        (context->json_request || (context->body_buf && context->body_len > 0))) {
        if (!context->json_request && context->body_buf && context->body_len > 0) {
            if (parse_json_request(gateway, context, context->body_buf, context->body_len) != 0) {
                return handle_parse_error(gateway, connection, context, context->body_len);
            }
        }
        return handle_post_jsonrpc(gateway, connection, context);
    }

    http_route_handler_t route_handler = find_http_route(method, url);
    if (route_handler)
        return route_handler(gateway, connection, context);
    return handle_not_found(gateway, connection, context);
}

/**
  * @brief HTTP request entry point
  *
  * Processing flow (4 phases), each staged in its own helper:
  * Phase 1: gw_rate_gate / gw_auth_gate / gw_ctx_new (first call)
  * Phase 2: gw_body_append (POST body accumulation)
  * Phase 3: gw_dispatch (JSON-RPC aggregation)
  * Phase 4: gw_dispatch (route table / 404 fallback)
 */
int handle_http_request(void *cls, struct MHD_Connection *connection, const char *url,
                        const char *method, const char *version,
                        const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    http_gateway_t *gateway = (http_gateway_t *)cls;
    http_request_context_t *context = (http_request_context_t *)*con_cls;

    int ret = MHD_YES;
    if (gw_rate_gate(gateway, connection, &ret) ||
        gw_auth_gate(gateway, connection, url, method, &ret))
        return ret;

    if (!context)
        return gw_ctx_new(gateway, connection, url, method, con_cls);

    if (strcmp(method, "POST") == 0 && upload_data && *upload_data_size > 0)
        return gw_body_append(gateway, connection, context, upload_data, upload_data_size);

    return gw_dispatch(gateway, connection, context, url, method);
}
