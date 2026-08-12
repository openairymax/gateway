// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_frame.c
 * @brief HTTP/2 网关帧解析域（nghttp2 会话回调与回调表构建）
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"

#ifdef AIRY_HAS_HTTP2

/**
 * @brief on_begin_headers 回调 — 分配流上下文
 */
int http2_on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame,
                           void *user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    http2_stream_context_t *ctx = http2_stream_create(frame->hd.stream_id);
    if (!ctx) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    int ret = nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, ctx);
    if (ret != 0) {
        http2_stream_destroy(ctx);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    (void)user_data;
    return 0;
}

/**
 * @brief on_header 回调 — 收集请求头
 */
int http2_on_header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name,
                    size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags,
                    void *user_data)
{
    (void)flags;
    (void)user_data;

    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session,
                                                                       frame->hd.stream_id);
    if (!ctx)
        return 0;

    if (namelen > 0 && name[0] == ':') {
        if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
            return http2_stream_set_header_str(&ctx->method, value, valuelen);
        }
        if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
            return http2_stream_set_header_str(&ctx->path, value, valuelen);
        }
        return 0;
    }

    if (namelen == 12 && strncasecmp((const char *)name, "content-type", 12) == 0) {
        return http2_stream_set_header_str(&ctx->content_type, value, valuelen);
    }
    if (namelen == 6 && strncasecmp((const char *)name, "origin", 6) == 0) {
        return http2_stream_set_header_str(&ctx->origin, value, valuelen);
    }

    return 0;
}

/**
 * @brief on_data_chunk_recv 回调 — 接收 DATA 帧数据块
 */
int http2_on_data_chunk_recv(nghttp2_session *session, uint8_t flags, int32_t stream_id,
                             const uint8_t *data, size_t len, void *user_data)
{
    (void)flags;

    http2_gateway_t *gw = (http2_gateway_t *)user_data;
    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (!ctx)
        return 0;

    /* P0: 请求体无上限时，超大 body 会在完整接收后才被 max_request_size
     * 校验拦截，导致内存耗尽 DoS。此处按 gw->base.max_request_size 在累加
     * 内存前限流，超限立即 RST_STREAM 终止该流。 */
    if (gw && ctx->request_body_len + len > gw->base.max_request_size) {
        LOG_WARN("request body exceeds limit: %zu + %zu > %zu (stream_id=%d)",
                 ctx->request_body_len, len, gw->base.max_request_size, stream_id);
        ctx->response_status = 413;
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
        return 0;
    }

    int ret = http2_stream_append_body(ctx, data, len);
    if (ret != 0) {
        LOG_ERROR("Failed to append body data: %d (stream_id=%d)", ret, stream_id);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    return 0;
}

/**
 * @brief on_frame_recv 回调 — 帧接收完成
 *
 * 检测 HEADERS 或 DATA 帧的 END_STREAM 标志，
 * 触发请求处理。
 */
int http2_on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) {
        return 0;
    }

    if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        return 0;
    }

    http2_process_request(session, frame->hd.stream_id, user_data);
    return 0;
}

/**
 * @brief on_stream_close 回调 — 流关闭，清理资源
 */
int http2_on_stream_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
                          void *user_data)
{
    (void)user_data;

    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (ctx) {
        if (error_code != NGHTTP2_NO_ERROR) {
            LOG_WARN("stream closed with error: stream_id=%d, error_code=%u (%s)", stream_id,
                     error_code, nghttp2_http2_strerror(error_code));
        } else {
            LOG_DEBUG("stream closed normally: stream_id=%d", stream_id);
        }
        http2_stream_destroy(ctx);
        nghttp2_session_set_stream_user_data(session, stream_id, NULL);
    }

    return 0;
}

/**
 * @brief 创建 nghttp2 回调表
 */
int http2_create_callbacks(nghttp2_session_callbacks **callbacks)
{
    int ret = nghttp2_session_callbacks_new(callbacks);
    if (ret != 0) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "nghttp2_session_callbacks_new failed");
        return ret;
    }

    nghttp2_session_callbacks_set_on_begin_headers_callback(*callbacks, http2_on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(*callbacks, http2_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(*callbacks, http2_on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_frame_recv_callback(*callbacks, http2_on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(*callbacks, http2_on_stream_close);

    return 0;
}

#endif /* AIRY_HAS_HTTP2 */
