// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_response.c
 * @brief HTTP/2 网关响应域（data provider 回调、响应提交与 JSON-RPC/健康/预检处理）
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"

#ifdef AIRY_HAS_HTTP2

/**
 * @brief 数据源读取回调 — 向 nghttp2 提供 HTTP/2 DATA 帧的响应体
 *
 * 从流上下文的 response_body 中读取数据，拷贝到 nghttp2 提供的 buf 中。
 * 当所有数据读取完毕时设置 NGHTTP2_DATA_FLAG_EOF。
 */
ssize_t http2_data_source_read_callback(nghttp2_session *session, int32_t stream_id,
                                        uint8_t *buf, size_t length, uint32_t *data_flags,
                                        nghttp2_data_source *source, void *user_data)
{
    (void)session;
    (void)stream_id;
    (void)source;

    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (!ctx) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t remaining = ctx->response_body_len - ctx->response_sent;
    if (remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t to_copy = remaining < length ? remaining : length;
    AIRY_MEMCPY(buf, ctx->response_body + ctx->response_sent, to_copy);
    ctx->response_sent += to_copy;

    if (ctx->response_sent >= ctx->response_body_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (ssize_t)to_copy;
}

/**
 * @brief 提交 HTTP/2 响应
 *
 * 构建响应头，设置 data_provider，调用 nghttp2_submit_response。
 * 响应体通过 data_source_read_callback 异步发送。
 */
int http2_submit_response_impl(nghttp2_session *session, http2_stream_context_t *ctx,
                               http2_gateway_t *gw)
{
    if (!ctx || !session) {
        return NGHTTP2_ERR_INVALID_ARGUMENT;
    }

    if (!ctx->response_body) {
        ctx->response_body = AIRY_STRDUP("{}");
        ctx->response_body_len = 2;
    }

    nghttp2_nv nva[16];
    size_t nvlen = http2_build_response_headers(ctx, gw, nva, 16);
    if (nvlen == (size_t)-1) {
        /* P0: 头构建 OOM — 已清理部分分配，不得提交 value=NULL 的非法 nv；
         * 终止该流，避免 nghttp2_submit_response 崩溃 */
        LOG_ERROR("failed to build response headers (stream_id=%d)", ctx->stream_id);
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, ctx->stream_id,
                                  NGHTTP2_INTERNAL_ERROR);
        ctx->response_sent_flag = true;
        return NGHTTP2_ERR_NOMEM;
    }

    nghttp2_data_provider data_prd;
    data_prd.source.ptr = ctx;
    data_prd.read_callback = http2_data_source_read_callback;

    int ret = nghttp2_submit_response(session, ctx->stream_id, nva, nvlen, &data_prd);

    http2_free_response_headers(nva, nvlen);

    if (ret != 0) {
        LOG_ERROR("nghttp2_submit_response failed: %s (stream_id=%d)", nghttp2_strerror(ret),
                  ctx->stream_id);
    } else {
        ctx->response_sent_flag = true;
    }

    return ret;
}

/**
 * @brief 处理 JSON-RPC POST 请求
 *
 * 复用 gateway_protocol_handle_request 进行多协议处理，
 * 回退到 gateway_rpc_handle_request 处理已解析的 JSON。
 */
char *http2_handle_jsonrpc(http2_gateway_t *gw, http2_stream_context_t *ctx)
{
    http_gateway_t *base = &gw->base;

    if (base->protocol_handler && ctx->request_body && ctx->request_body_len > 0) {
        http2_handler_adapter_t adapter = {.internal_handler = base->handler,
                                           .internal_data = base->handler_data};

        rpc_result_t result =
            gateway_protocol_handle_request(base->protocol_handler, (const char *)ctx->request_body,
                                            ctx->request_body_len, AIRY_PROTOCOL_COUNT,
                                            base->handler ? http2_internal_handler_adapter : NULL,
                                            base->handler ? &adapter : NULL);

        if (result.error_code != 0 || !result.response_json) {
            char *error_resp =
                result.response_json ?
                    result.response_json :
                    jsonrpc_create_error_response(NULL, -32603, "Internal error", NULL);
            if (result.response_json)
                result.response_json = NULL;
            gateway_rpc_free(&result);
            return error_resp;
        }

        char *success_resp = result.response_json;
        result.response_json = NULL;
        gateway_rpc_free(&result);
        return success_resp;
    }

    return jsonrpc_create_error_response(NULL, -32600, "Invalid request", NULL);
}

/**
 * @brief 处理健康检查请求 GET /health
 */
char *http2_handle_health(void)
{
    return AIRY_STRDUP("{\"status\":\"healthy\",\"service\":\"gateway\",\"protocol\":\"h2\"}");
}

/**
 * @brief 处理 OPTIONS 预检请求
 */
char *http2_handle_preflight(void)
{
    return AIRY_STRDUP("");
}

#endif /* AIRY_HAS_HTTP2 */
