// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file http2_gateway.c
 * @brief HTTP/2 gateway implementation - nghttp2-based HTTP/2 server.
 *
 * Implements a full HTTP/2 protocol server, reusing the HTTP/1.1 gateway's
 * protocol handling (gateway_protocol_handle_request / gateway_rpc_handle_request).
 *
 * Core flow:
 *   1. TCP listening socket (non-blocking)
 *   2. poll() event loop (dedicated pthread)
 *   3. one nghttp2 server session per connection
 *   4. nghttp2 callbacks collect HTTP/2 request headers and DATA frames
 *   5. END_STREAM triggers request handling -> JSON response
 *   6. nghttp2_submit_response submits the response (data_provider callback feeds data)
 *
 * IRON-2 rule: no stubs or simplified features; a truly usable HTTP/2 server.
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"

#ifdef AIRY_HAS_HTTP2

static int http2_gateway_start_impl(void *impl);
static void http2_gateway_stop_impl(void *impl);
static void http2_gateway_destroy_impl(void *impl);

static airy_err_t http2_gateway_start_impl(void *impl)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;

    if (atomic_load(&gw->running)) {
        AIRY_LOG_WARN("HTTP/2 gateway already running");
        return AIRY_EBUSY;
    }

    gw->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gw->listen_fd < 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__, "socket creation failed: %s",
                         strerror(errno));
        return AIRY_EBUSY;
    }

    http2_set_reuseaddr(gw->listen_fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(gw->base.port);

    if (inet_pton(AF_INET, gw->base.host, &addr.sin_addr) != 1) {
        /* P2: previously a non-IPv4 host (e.g. a hostname) silently fell back
         * to INADDR_ANY, exposing the gateway on every interface. Resolve the
         * hostname instead, and fail loudly if it cannot be resolved. */
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo *res = NULL;
        int rc = getaddrinfo(gw->base.host, NULL, &hints, &res);
        if (rc != 0 || !res || !res->ai_addr) {
            airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__,
                             "cannot resolve host '%s': %s", gw->base.host,
                             rc != 0 ? gai_strerror(rc) : "no addresses");
            close(gw->listen_fd);
            gw->listen_fd = -1;
            return AIRY_EBUSY;
        }

        addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    if (bind(gw->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__, "bind failed on %s:%u: %s",
                         gw->base.host, gw->base.port, strerror(errno));
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_EBUSY;
    }

    if (listen(gw->listen_fd, HTTP2_LISTEN_BACKLOG) < 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__, "listen failed: %s",
                         strerror(errno));
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_EBUSY;
    }

    if (http2_set_nonblocking(gw->listen_fd) < 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__,
                         "failed to set non-blocking on listen socket");
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_EBUSY;
    }

    atomic_store(&gw->running, true);

    pthread_t *thread = AIRY_MALLOC(sizeof(pthread_t));
    if (!thread) {
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "thread allocation failed");
        atomic_store(&gw->running, false);
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    int ret = pthread_create(thread, NULL, http2_event_loop, gw);
    if (ret != 0) {
        airy_err_push_ex(AIRY_ERR_IO, __FILE__, __LINE__, __func__, "pthread_create failed: %s",
                         strerror(ret));
        AIRY_FREE(thread);
        atomic_store(&gw->running, false);
        close(gw->listen_fd);
        gw->listen_fd = -1;
        return AIRY_EBUSY;
    }

    gw->event_thread = thread;

    AIRY_LOG_INFO("HTTP/2 gateway started on %s:%u (max_streams=%u)", gw->base.host, gw->base.port,
             gw->max_concurrent_streams);

    return AIRY_SUCCESS;
}

static void http2_gateway_stop_impl(void *impl)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;

    if (!atomic_load(&gw->running)) {
        return;
    }

    atomic_store(&gw->running, false);

    if (gw->listen_fd >= 0) {
        close(gw->listen_fd);
        gw->listen_fd = -1;
    }

    if (gw->event_thread) {
        pthread_t *thread = (pthread_t *)gw->event_thread;
        pthread_join(*thread, NULL);
        AIRY_FREE(thread);
        gw->event_thread = NULL;
    }

    AIRY_LOG_INFO("HTTP/2 gateway stopped");
}

static void http2_gateway_destroy_impl(void *impl)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;
    if (!gw)
        return;

    http2_gateway_stop_impl(gw);

    if (gw->sessions) {
        for (size_t i = 0; i < gw->session_count; i++) {
            http2_session_destroy(gw->sessions[i]);
        }
        AIRY_FREE(gw->sessions);
        gw->sessions = NULL;
    }
    gw->session_count = 0;
    gw->session_capacity = 0;

    http_gateway_base_destroy(&gw->base);

    AIRY_FREE(gw);
}

/**
  * @brief HTTP/2 gateway operations table
 */
static const gateway_ops_t http2_gateway_ops = {
    .start = http2_gateway_start_impl,
    .destroy = http2_gateway_destroy_impl,
};

gateway_t *http2_gateway_create(const char *host, uint16_t port)
{
    if (!host) {
        return NULL;
    }

    http2_gateway_t *gw = AIRY_CALLOC(1, sizeof(http2_gateway_t));
    if (!gw) {
        return NULL;
    }

    http_gateway_t *base = &gw->base;
    if (!http_gateway_base_init(base, host, port)) {
        AIRY_FREE(gw);
        return NULL;
    }

    /* HTTP/2 侧协议处理器无条件启用（HTTP/1.1 侧按环境开关按需）。 */
    base->protocol_handler = gateway_protocol_handler_create(NULL);

    gw->listen_fd = -1;
    gw->sessions = NULL;
    gw->session_count = 0;
    gw->session_capacity = 0;
    gw->event_thread = NULL;
    gw->max_concurrent_streams = HTTP2_DEFAULT_MAX_STREAMS;
    gw->connection_timeout = HTTP2_DEFAULT_TIMEOUT_SEC;

    const char *env_streams = getenv("GATEWAY_HTTP2_MAX_STREAMS");
    if (env_streams) {
        unsigned long v = strtoul(env_streams, NULL, 10);
        if (v > 0 && v <= 1000) {
            gw->max_concurrent_streams = (unsigned int)v;
        }
    }

    const char *env_timeout = getenv("GATEWAY_HTTP2_TIMEOUT");
    if (env_timeout) {
        unsigned long v = strtoul(env_timeout, NULL, 10);
        if (v > 0) {
            gw->connection_timeout = (unsigned int)v;
        }
    }

    atomic_init(&gw->running, false);

    gateway_t *gateway = AIRY_MALLOC(sizeof(gateway_t));
    if (!gateway) {
        http_gateway_base_destroy(base);
        AIRY_FREE(gw);
        return NULL;
    }

    gateway->ops = &http2_gateway_ops;
    gateway->impl = gw;
    gateway->type = GATEWAY_TYPE_HTTP;
    base->owner = gateway;

    AIRY_LOG_INFO("HTTP/2 gateway created on %s:%u", host, port);

    return gateway;
}

#endif /* AIRY_HAS_HTTP2 */
#ifndef AIRY_HAS_HTTP2

gateway_t *http2_gateway_create(const char *host,
                                uint16_t port)
{
    AIRY_LOG_WARN("HTTP/2 gateway not available: nghttp2 not compiled in");
    return NULL;
}

#endif /* !AIRY_HAS_HTTP2 */
