// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_stream.c
 * @brief HTTP/2 网关流处理域（流上下文生命周期、请求体缓冲、header 复制与内部处理器适配）
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"

#ifdef AIRY_HAS_HTTP2

/**
 * @brief 创建 HTTP/2 流上下文
 */
http2_stream_context_t *http2_stream_create(int32_t stream_id)
{
    http2_stream_context_t *ctx = AIRY_CALLOC(1, sizeof(http2_stream_context_t));
    if (!ctx) {
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "stream context allocation failed");
        return NULL;
    }
    ctx->stream_id = stream_id;
    ctx->response_status = 200;
    LOG_DEBUG("stream created: stream_id=%d", stream_id);
    return ctx;
}

/**
 * @brief 销毁 HTTP/2 流上下文
 */
void http2_stream_destroy(http2_stream_context_t *ctx)
{
    if (!ctx)
        return;

    LOG_DEBUG("stream destroyed: stream_id=%d, req_body=%zuB, resp_body=%zuB, status=%d",
              ctx->stream_id, ctx->request_body_len, ctx->response_body_len, ctx->response_status);

    AIRY_FREE(ctx->method);
    AIRY_FREE(ctx->path);
    AIRY_FREE(ctx->content_type);
    AIRY_FREE(ctx->origin);
    AIRY_FREE(ctx->request_body);
    AIRY_FREE(ctx->response_body);
    AIRY_FREE(ctx);
}

/**
 * @brief 追加数据到请求体缓冲区
 * @return 0 成功，负数错误码
 */
int http2_stream_append_body(http2_stream_context_t *ctx, const uint8_t *data, size_t len)
{
    if (!ctx || (!data && len > 0))
        return AIRY_ERR_NULL_POINTER;

    if (len == 0)
        return 0;

    if (ctx->request_body_len + len > ctx->request_body_cap) {
        size_t new_cap = ctx->request_body_cap == 0 ? 4096 : ctx->request_body_cap;
        while (new_cap < ctx->request_body_len + len) {
            new_cap *= 2;
            if (new_cap == 0) {
                airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                                 "body buffer overflow");
                return AIRY_ERR_OUT_OF_MEMORY;
            }
        }
        uint8_t *new_buf = AIRY_REALLOC(ctx->request_body, new_cap);
        if (!new_buf) {
            airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                             "body buffer realloc failed");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        ctx->request_body = new_buf;
        ctx->request_body_cap = new_cap;
    }

    AIRY_MEMCPY(ctx->request_body + ctx->request_body_len, data, len);
    ctx->request_body_len += len;
    return 0;
}

/**
 * @brief 安全复制 header 值字符串
 */
int http2_stream_set_header_str(char **dst, const uint8_t *value, size_t vlen)
{
    if (!dst || !value)
        return AIRY_ERR_NULL_POINTER;

    char *copy = AIRY_MALLOC(vlen + 1);
    if (!copy) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    AIRY_MEMCPY(copy, value, vlen);
    copy[vlen] = '\0';

    AIRY_FREE(*dst);
    *dst = copy;
    return 0;
}

/**
 * @brief 适配器：将 gateway_internal_handler_t 转换为 protocol_handle_request 的回调签名
 */
int http2_internal_handler_adapter(const char *request_json, char **response_json,
                                   void *user_data)
{
    http2_handler_adapter_t *adapter = (http2_handler_adapter_t *)user_data;
    if (!adapter || !adapter->internal_handler) {
        *response_json = NULL;
        airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__,
                         "adapter or internal handler is NULL");
        return AIRY_ERR_NULL_POINTER;
    }

    char *resp = adapter->internal_handler((void *)request_json, adapter->internal_data);
    if (resp) {
        *response_json = resp;
        return 0;
    }
    *response_json = NULL;
    airy_err_push_ex(AIRY_ERR_NULL_POINTER, __FILE__, __LINE__, __func__,
                     "internal handler returned NULL");
    return AIRY_ERR_NULL_POINTER;
}

#endif /* AIRY_HAS_HTTP2 */
