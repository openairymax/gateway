// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_frame.c
 * @brief HTTP/2 gateway frame parsing domain (nghttp2 session callbacks and callback-table building).
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"

#ifdef AIRY_HAS_HTTP2

/**
  * @brief on_begin_headers callback - allocate a stream context
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
  * @brief on_header callback - collect request headers
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
  * @brief on_data_chunk_recv callback - receive DATA frame chunks
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

    /* P0: without a body limit, an oversized body would only be rejected by
     * max_request_size after being fully received, enabling memory-exhaustion DoS.
     * Throttle by gw->base.max_request_size before accumulating memory and
     * immediately RST_STREAM the stream when the limit is exceeded. */
    if (gw && ctx->request_body_len + len > gw->base.max_request_size) {
        AIRY_LOG_WARN("request body exceeds limit: %zu + %zu > %zu (stream_id=%d)",
                 ctx->request_body_len, len, gw->base.max_request_size, stream_id);
        ctx->response_status = 413;
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
        return 0;
    }

    int ret = http2_stream_append_body(ctx, data, len);
    if (ret != 0) {
        AIRY_LOG_ERROR("Failed to append body data: %d (stream_id=%d)", ret, stream_id);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    return 0;
}

/**
  * @brief on_frame_recv callback - frame receive complete
 *
 * Detects the END_STREAM flag on HEADERS or DATA frames and triggers
 * request handling.
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
  * @brief on_stream_close callback - stream closed, free resources
 */
int http2_on_stream_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
                          void *user_data)
{
    (void)user_data;

    http2_stream_context_t *ctx =
        (http2_stream_context_t *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (ctx) {
        if (error_code != NGHTTP2_NO_ERROR) {
            AIRY_LOG_WARN("stream closed with error: stream_id=%d, error_code=%u (%s)", stream_id,
                     error_code, nghttp2_http2_strerror(error_code));
        } else {
            AIRY_LOG_DEBUG("stream closed normally: stream_id=%d", stream_id);
        }
        http2_stream_destroy(ctx);
        nghttp2_session_set_stream_user_data(session, stream_id, NULL);
    }

    return 0;
}

/**
  * @brief Build the nghttp2 callback table
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
