// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_route.c
 * @brief HTTP/2 gateway request routing domain (method/path dispatch and response submission).
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"

#ifdef AIRY_HAS_HTTP2

/**
  * @brief Handle the complete request and submit a response.
 *
 * Called after END_STREAM; routes to the matching handler by method + path,
 * generates the JSON response, then submits it via nghttp2_submit_response.
 */
void http2_process_request(nghttp2_session *session, int32_t stream_id, void *user_data)
{
    http2_gateway_session_t *sess = (http2_gateway_session_t *)user_data;
    if (!sess || !sess->gateway) {
        AIRY_LOG_ERROR("process_request: invalid session (stream_id=%d)", stream_id);
        return;
    }
    http2_gateway_t *gw = sess->gateway;
    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);

    if (!ctx || ctx->response_sent_flag) {
        AIRY_LOG_WARN("process_request skipped: ctx=%p, already_sent=%d", (void *)ctx,
                 ctx ? ctx->response_sent_flag : 0);
        return;
    }

    char *response_json = NULL;

    AIRY_LOG_INFO("processing request: stream_id=%d, method=%s, path=%s, body_len=%zu", ctx->stream_id,
             ctx->method ? ctx->method : "(null)", ctx->path ? ctx->path : "(null)",
             ctx->request_body_len);

    if (gw->base.rate_limiter && sess->client_ip[0] &&
        !gateway_rate_limiter_allow(gw->base.rate_limiter, sess->client_ip)) {
        AIRY_LOG_WARN("rate limit exceeded for %s (stream_id=%d)", sess->client_ip, ctx->stream_id);
        ctx->response_status = 429;
        response_json = jsonrpc_create_error_response(NULL, -32004, "Rate limit exceeded", NULL);
    } else if (ctx->method && strcmp(ctx->method, "POST") == 0) {
        /* POST / → JSON-RPC */
        if (ctx->request_body_len > gw->base.max_request_size) {
            AIRY_LOG_WARN("request too large: %zu > %zu (stream_id=%d)", ctx->request_body_len,
                     gw->base.max_request_size, ctx->stream_id);
            ctx->response_status = 413;
            response_json = jsonrpc_create_error_response(NULL, -413, "Request too large", NULL);
        } else {
            response_json = http2_handle_jsonrpc(gw, ctx);
            if (!response_json) {
                AIRY_LOG_ERROR("jsonrpc handler returned NULL (stream_id=%d)", ctx->stream_id);
                ctx->response_status = 500;
                response_json = jsonrpc_create_error_response(NULL, -32603, "Internal error", NULL);
            }
        }
    } else if (ctx->method && strcmp(ctx->method, "GET") == 0) {
        if (ctx->path && strcmp(ctx->path, "/health") == 0) {
            AIRY_LOG_DEBUG("health check request (stream_id=%d)", ctx->stream_id);
            response_json = http2_handle_health();
        } else {
            AIRY_LOG_WARN("GET path not found: %s (stream_id=%d)", ctx->path ? ctx->path : "(null)",
                     ctx->stream_id);
            ctx->response_status = 404;
            response_json = jsonrpc_create_error_response(NULL, -32601, "Not Found", NULL);
        }
    } else if (ctx->method && strcmp(ctx->method, "OPTIONS") == 0) {
        AIRY_LOG_DEBUG("OPTIONS preflight request (stream_id=%d)", ctx->stream_id);
        response_json = http2_handle_preflight();
    } else {
        AIRY_LOG_WARN("unsupported method: %s (stream_id=%d)", ctx->method ? ctx->method : "(null)",
                 ctx->stream_id);
        ctx->response_status = 404;
        response_json = jsonrpc_create_error_response(NULL, -32601, "Not Found", NULL);
    }

    if (response_json) {
        ctx->response_body = response_json;
        ctx->response_body_len = strlen(response_json);
    } else {
        ctx->response_body = AIRY_STRDUP("{}");
        ctx->response_body_len = 2;
    }

    ctx->response_sent = 0;

    atomic_fetch_add(&gw->base.requests_total, 1);
    atomic_fetch_add(&gw->base.bytes_received, ctx->request_body_len);
    atomic_fetch_add(&gw->base.bytes_sent, ctx->response_body_len);

    int submit_ret = http2_submit_response_impl(session, ctx, gw);
    AIRY_LOG_INFO("response submitted: stream_id=%d, status=%d, resp_len=%zu, ret=%d", ctx->stream_id,
             ctx->response_status, ctx->response_body_len, submit_ret);
}

#endif /* AIRY_HAS_HTTP2 */
