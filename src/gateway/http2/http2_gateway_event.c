// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_event.c
 * @brief HTTP/2 gateway event loop domain (accept, timeout cleanup, poll loop, socket options).
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"
#include "platform.h"

#ifdef AIRY_HAS_HTTP2

/**
 * @brief Accept a new connection
 */
void http2_event_loop_accept(http2_gateway_t *gw)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int fd = accept(gw->listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            AIRY_LOG_ERROR("accept failed: %s", strerror(errno));
        }
        return;
    }

    if (gw->session_count >= gw->max_concurrent_streams) {
        AIRY_LOG_WARN("connection limit reached (%zu/%u), rejecting new connection", gw->session_count,
                 gw->max_concurrent_streams);
        close(fd);
        return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    http2_gateway_session_t *sess = http2_session_create(gw, fd);
    if (!sess) {
        close(fd);
        return;
    }

    if (http2_session_send_data(sess) != 0) {
        http2_session_destroy(sess);
        return;
    }

    if (http2_gateway_add_session(gw, sess) != 0) {
        http2_session_destroy(sess);
        return;
    }

    char ip_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf));
    AIRY_LOG_INFO("connection accepted: %s:%d → fd=%d (sessions=%zu/%u)", ip_buf, ntohs(addr.sin_port),
             fd, gw->session_count, gw->max_concurrent_streams);
}

/**
  * @brief Check session timeouts and clean up
 */
void http2_event_loop_cleanup(http2_gateway_t *gw)
{
    uint64_t now = gateway_time_ns();
    uint64_t timeout_ns = (uint64_t)gw->connection_timeout * 1000000000ULL;

    for (size_t i = 0; i < gw->session_count;) {
        http2_gateway_session_t *sess = gw->sessions[i];

        bool should_close = sess->closing;

        if (!should_close && timeout_ns > 0) {
            if ((now - sess->last_activity_ns) > timeout_ns) {
                AIRY_LOG_INFO("session timeout: fd=%d, idle=%llums", sess->fd,
                         (unsigned long long)((now - sess->last_activity_ns) / 1000000ULL));
                should_close = true;
            }
        }

        if (!should_close) {
            if (!nghttp2_session_want_read(sess->session) &&
                !nghttp2_session_want_write(sess->session)) {
                AIRY_LOG_DEBUG("nghttp2 session done: fd=%d (no more read/write)", sess->fd);
                should_close = true;
            }
        }

        if (should_close) {

            if (sess->pending_send_buf && sess->pending_send_offset < sess->pending_send_len) {
                AIRY_LOG_WARN("closing session with %zu bytes unsent: fd=%d",
                         sess->pending_send_len - sess->pending_send_offset, sess->fd);
            }

            if (sess->session) {
                nghttp2_submit_goaway(sess->session, NGHTTP2_FLAG_NONE,
                                      nghttp2_session_get_last_proc_stream_id(sess->session),
                                      NGHTTP2_NO_ERROR, NULL, 0);
                http2_session_send_data(sess);
            }
            http2_gateway_remove_session(gw, i);
        } else {
            i++;
        }
    }
}

/* pollfd 装配：槽 0 固定为监听套接字，其后按会话表顺序排列 */
static nfds_t h2e_fill(http2_gateway_t *gw, struct pollfd *fds)
{
    nfds_t nfds = 0;

    fds[nfds].fd = gw->listen_fd;
    fds[nfds].events = POLLIN;
    nfds++;

    for (size_t i = 0; i < gw->session_count; i++) {
        http2_gateway_session_t *sess = gw->sessions[i];
        short events = 0;

        if (nghttp2_session_want_read(sess->session))
            events |= POLLIN;
        if (nghttp2_session_want_write(sess->session))
            events |= POLLOUT;
        if (sess->pending_send_buf && sess->pending_send_offset < sess->pending_send_len)
            events |= POLLOUT;
        if (events == 0)
            events = POLLIN;

        fds[nfds].fd = sess->fd;
        fds[nfds].events = events;
        nfds++;
    }
    return nfds;
}

/* 读事件：0 表示会话可继续，-1 表示需关闭 */
static int h2e_read(http2_gateway_session_t *sess)
{
    int rc = http2_session_recv_data(sess);
    return (rc < 0 || rc == 1) ? -1 : 0;
}

/* 单个会话的事件处理：返回 0 继续、1 移除（关闭优先于出站刷新） */
static int h2e_session(http2_gateway_t *gw, size_t i, short revents)
{
    http2_gateway_session_t *sess = gw->sessions[i];
    bool ok = true;

    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        AIRY_LOG_WARN("session socket error: fd=%d, revents=0x%x (%s%s%s)", sess->fd, revents,
                 (revents & POLLERR) ? "ERR " : "", (revents & POLLHUP) ? "HUP " : "",
                 (revents & POLLNVAL) ? "NVAL" : "");
        return 1;
    }

    if (revents & POLLIN)
        ok = h2e_read(sess) == 0;
    if (ok && (revents & POLLOUT))
        ok = http2_session_send_data(sess) == 0;
    if (ok && (revents & POLLIN))
        ok = http2_session_send_data(sess) == 0;
    return ok ? 0 : 1;
}

/* 监听套接字事件：返回 0 继续、-1 退出主循环 */
static int h2e_listen(http2_gateway_t *gw, short revents)
{
    if (revents & POLLIN)
        http2_event_loop_accept(gw);
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        AIRY_LOG_ERROR("Listen socket error");
        return -1;
    }
    return 0;
}

/* poll 结果分派：返回 0 继续、-1 退出主循环 */
static int h2e_dispatch(http2_gateway_t *gw, struct pollfd *fds, nfds_t nfds)
{
    if (h2e_listen(gw, fds[0].revents) != 0)
        return -1;

    for (size_t i = 0; i < gw->session_count && i + 1 < nfds;) {
        if (h2e_session(gw, i, fds[i + 1].revents) == 0) {
            i++;
            continue;
        }
        /* remove_session() 把末位会话换入槽 i，清零 revents 避免用被移除
         * 会话的旧事件处理新换入的会话（P1：活跃连接被随机关闭）。 */
        http2_gateway_remove_session(gw, i);
        fds[i + 1].revents = 0;
    }
    return 0;
}

/* 一轮事件循环：返回 0 继续、-1 退出主循环 */
static int h2e_round(http2_gateway_t *gw)
{
    struct pollfd *fds = AIRY_CALLOC(gw->session_count + 1, sizeof(struct pollfd));
    if (!fds) {
        airy_sleep_ms(1000);
        return 0;
    }

    nfds_t nfds = h2e_fill(gw, fds);
    int ret = poll(fds, nfds, HTTP2_POLL_TIMEOUT_MS);
    int rc = 0;

    if (ret < 0) {
        if (errno != EINTR) {
            AIRY_LOG_ERROR("poll failed: %s", strerror(errno));
            rc = -1;
        }
    } else {
        rc = h2e_dispatch(gw, fds, nfds);
        http2_event_loop_cleanup(gw);
    }

    AIRY_FREE(fds);
    return rc;
}

/**
 * @brief Main event loop (runs on a dedicated thread)
 */
void *http2_event_loop(void *arg)
{
    http2_gateway_t *gw = (http2_gateway_t *)arg;

    AIRY_LOG_INFO("HTTP/2 event loop started (port=%u)", gw->base.port);

    while (atomic_load(&gw->running)) {
        if (h2e_round(gw) != 0)
            break;
    }

    while (gw->session_count > 0) {
        http2_gateway_remove_session(gw, 0);
    }

    AIRY_LOG_INFO("HTTP/2 event loop stopped");
    return NULL;
}

/**
  * @brief Set a socket to non-blocking mode
 */
int http2_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
  * @brief Set SO_REUSEADDR
 */
void http2_set_reuseaddr(int fd)
{
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

#endif /* AIRY_HAS_HTTP2 */
