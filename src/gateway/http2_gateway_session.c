// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_session.c
 * @brief HTTP/2 网关会话管理域（nghttp2 会话创建/销毁/收发/增删）
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"

#ifdef AIRY_HAS_HTTP2

/**
 * @brief 创建新的 HTTP/2 会话
 */
http2_gateway_session_t *http2_session_create(http2_gateway_t *gw, int fd)
{
    http2_gateway_session_t *sess = AIRY_CALLOC(1, sizeof(http2_gateway_session_t));
    if (!sess) {
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "session allocation failed");
        return NULL;
    }

    sess->fd = fd;
    sess->gateway = gw;
    sess->connect_time_ns = gateway_time_ns();
    sess->last_activity_ns = sess->connect_time_ns;
    sess->closing = false;

    nghttp2_session_callbacks *callbacks = NULL;
    if (http2_create_callbacks(&callbacks) != 0) {
        AIRY_FREE(sess);
        return NULL;
    }

    int ret = nghttp2_session_server_new(&sess->session, callbacks, gw);
    nghttp2_session_callbacks_del(callbacks);

    if (ret != 0) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__,
                         "nghttp2_session_server_new failed: %s", nghttp2_strerror(ret));
        AIRY_FREE(sess);
        return NULL;
    }

    nghttp2_settings_entry settings_entries[3];
    size_t num_entries = 0;

    settings_entries[num_entries].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    settings_entries[num_entries].value = gw->max_concurrent_streams;
    num_entries++;

    settings_entries[num_entries].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
    settings_entries[num_entries].value = HTTP2_INITIAL_WINDOW_SIZE;
    num_entries++;

    settings_entries[num_entries].settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
    settings_entries[num_entries].value = HTTP2_MAX_HEADER_SIZE;
    num_entries++;

    ret = nghttp2_submit_settings(sess->session, NGHTTP2_FLAG_NONE, settings_entries, num_entries);
    if (ret != 0) {
        LOG_ERROR("nghttp2_submit_settings failed: %s (fd=%d)", nghttp2_strerror(ret), fd);
        nghttp2_session_del(sess->session);
        AIRY_FREE(sess);
        return NULL;
    }

    LOG_INFO("HTTP/2 session created: fd=%d, max_streams=%u, window=%d", fd,
             gw->max_concurrent_streams, HTTP2_INITIAL_WINDOW_SIZE);
    return sess;
}

/**
 * @brief 销毁 HTTP/2 会话
 */
void http2_session_destroy(http2_gateway_session_t *sess)
{
    if (!sess)
        return;

    LOG_INFO("HTTP/2 session destroying: fd=%d", sess->fd);

    if (sess->session) {
        nghttp2_session_del(sess->session);
        sess->session = NULL;
    }

    if (sess->fd >= 0) {
        close(sess->fd);
        sess->fd = -1;
    }

    if (sess->pending_send_buf) {
        AIRY_FREE(sess->pending_send_buf);
        sess->pending_send_buf = NULL;
        sess->pending_send_len = 0;
        sess->pending_send_offset = 0;
    }

    AIRY_FREE(sess);
}

/**
 * @brief 从 socket 读取数据并送入 nghttp2 处理
 * @return 0 正常，1 连接关闭，负数错误
 */
int http2_session_recv_data(http2_gateway_session_t *sess)
{
    uint8_t buf[HTTP2_RECV_BUF_SIZE];

    ssize_t nread = read(sess->fd, buf, sizeof(buf));
    if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        LOG_ERROR("read failed on fd %d: %s", sess->fd, strerror(errno));
        return -1;
    }

    if (nread == 0) {

        LOG_INFO("peer closed connection: fd=%d", sess->fd);
        return 1;
    }

    sess->last_activity_ns = gateway_time_ns();
    atomic_fetch_add(&sess->gateway->base.bytes_received, (uint64_t)nread);
    LOG_DEBUG("recv %zd bytes on fd=%d", nread, sess->fd);

    ssize_t processed = nghttp2_session_mem_recv(sess->session, buf, (size_t)nread);
    if (processed < 0) {
        LOG_ERROR("nghttp2_session_mem_recv failed: %s (fd=%d, processed=%zd/%zd)",
                  nghttp2_strerror((int)processed), sess->fd, processed, nread);
        return -1;
    }

    if ((size_t)processed < (size_t)nread) {
        LOG_DEBUG("nghttp2 partial recv: processed=%zd/%zd bytes (fd=%d)", processed, nread,
                  sess->fd);
    }

    return 0;
}

/**
 * @brief 将 nghttp2 待发送数据写入 socket
 *
 * P0 修复: 原实现在 write() 部分写入时直接调用 nghttp2_session_mem_send()
 * 获取下一块数据，导致未写完的数据丢失。修复方案：
 *   1. 先尝试 flush pending_send_buf 中的残留数据
 *   2. 只有当 pending_send_buf 为空时才调用 nghttp2_session_mem_send
 *   3. 如果 write() 部分写入，将剩余数据缓存到 pending_send_buf
 *   4. 下次 POLLOUT 时从 pending_send_buf 继续发送
 *
 * @return 0 正常，负数错误
 */
int http2_session_send_data(http2_gateway_session_t *sess)
{

    if (sess->pending_send_buf && sess->pending_send_offset < sess->pending_send_len) {
        size_t remaining = sess->pending_send_len - sess->pending_send_offset;
        ssize_t written =
            write(sess->fd, sess->pending_send_buf + sess->pending_send_offset, remaining);

        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                LOG_DEBUG("pending flush deferred: fd=%d, remaining=%zu", sess->fd, remaining);
                return 0;
            }
            LOG_ERROR("pending write failed on fd %d: %s", sess->fd, strerror(errno));
            return -1;
        }

        sess->pending_send_offset += (size_t)written;
        sess->last_activity_ns = gateway_time_ns();
        atomic_fetch_add(&sess->gateway->base.bytes_sent, (uint64_t)written);
        LOG_DEBUG("pending flush: wrote %zd/%zu bytes (fd=%d)", written, remaining, sess->fd);

        if (sess->pending_send_offset < sess->pending_send_len) {

            return 0;
        }

        AIRY_FREE(sess->pending_send_buf);
        sess->pending_send_buf = NULL;
        sess->pending_send_len = 0;
        sess->pending_send_offset = 0;
    }

    const uint8_t *data_ptr = NULL;
    ssize_t send_len = nghttp2_session_mem_send(sess->session, &data_ptr);

    while (send_len > 0 && data_ptr) {
        size_t total = (size_t)send_len;
        size_t offset = 0;

        while (offset < total) {
            ssize_t written = write(sess->fd, data_ptr + offset, total - offset);

            if (written < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {

                    size_t remaining = total - offset;
                    sess->pending_send_buf = AIRY_MALLOC(remaining);
                    if (!sess->pending_send_buf) {
                        LOG_ERROR("pending buffer alloc failed: fd=%d, size=%zu", sess->fd,
                                  remaining);
                        return -1;
                    }
                    memcpy(sess->pending_send_buf, data_ptr + offset, remaining);
                    sess->pending_send_len = remaining;
                    sess->pending_send_offset = 0;
                    LOG_WARN("partial write: buffered %zu bytes for fd=%d (total=%zu, sent=%zu)",
                             remaining, sess->fd, total, offset);
                    return 0;
                }
                LOG_ERROR("write failed on fd %d: %s", sess->fd, strerror(errno));
                return -1;
            }

            offset += (size_t)written;
        }

        sess->last_activity_ns = gateway_time_ns();
        atomic_fetch_add(&sess->gateway->base.bytes_sent, (uint64_t)total);
        LOG_DEBUG("send %zu bytes on fd=%d", total, sess->fd);

        send_len = nghttp2_session_mem_send(sess->session, &data_ptr);
    }

    if (send_len < 0) {
        LOG_ERROR("nghttp2_session_mem_send failed: %s (fd=%d)", nghttp2_strerror((int)send_len),
                  sess->fd);
        return -1;
    }

    return 0;
}

/**
 * @brief 添加会话到网关的会话数组
 */
int http2_gateway_add_session(http2_gateway_t *gw, http2_gateway_session_t *sess)
{
    if (gw->session_count >= gw->session_capacity) {
        size_t new_cap = gw->session_capacity == 0 ? 16 : gw->session_capacity * 2;
        http2_gateway_session_t **new_arr =
            AIRY_REALLOC(gw->sessions, new_cap * sizeof(http2_gateway_session_t *));
        if (!new_arr) {
            airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                             "session array realloc failed");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        gw->sessions = new_arr;
        gw->session_capacity = new_cap;
    }

    gw->sessions[gw->session_count++] = sess;
    return 0;
}

/**
 * @brief 从会话数组中移除并销毁指定索引的会话
 */
void http2_gateway_remove_session(http2_gateway_t *gw, size_t index)
{
    if (index >= gw->session_count)
        return;

    http2_session_destroy(gw->sessions[index]);

    gw->session_count--;
    if (index < gw->session_count) {
        gw->sessions[index] = gw->sessions[gw->session_count];
    }
    gw->sessions[gw->session_count] = NULL;
}

#endif /* AIRY_HAS_HTTP2 */
