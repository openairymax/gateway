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
static const char *http2_gateway_get_name_impl(void *impl);
static airy_err_t http2_gateway_get_stats_impl(void *impl, char **out_json);
static bool http2_gateway_is_running_impl(void *impl);
static airy_err_t http2_gateway_set_handler_impl(void *impl, gateway_internal_handler_t handler,
                                                 void *user_data);

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

        addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

    http_gateway_t *base = &gw->base;

    if (base->handler_adapter) {
        AIRY_FREE(base->handler_adapter);
        base->handler_adapter = NULL;
    }
    base->handler = NULL;
    base->handler_data = NULL;

    if (base->host) {
        AIRY_FREE(base->host);
    }

    if (base->cors.allowed_methods) {
        AIRY_FREE(base->cors.allowed_methods);
    }
    if (base->cors.allowed_headers) {
        AIRY_FREE(base->cors.allowed_headers);
    }
    if (base->cors.allowed_origins) {
        for (size_t i = 0; i < base->cors.allowed_origins_count; i++) {
            AIRY_FREE(base->cors.allowed_origins[i]);
        }
        AIRY_FREE(base->cors.allowed_origins);
    }

    if (base->rate_limiter) {
        gateway_rate_limiter_destroy(base->rate_limiter);
    }

    if (base->protocol_handler) {
        gateway_protocol_handler_destroy(base->protocol_handler);
        base->protocol_handler = NULL;
    }

    if (base->dynamic_endpoints) {
        for (size_t i = 0; i < base->dynamic_endpoint_count; i++) {
            AIRY_FREE(base->dynamic_endpoints[i].method);
            AIRY_FREE(base->dynamic_endpoints[i].path);
        }
        AIRY_FREE(base->dynamic_endpoints);
        base->dynamic_endpoints = NULL;
    }
    base->dynamic_endpoint_count = 0;
    base->dynamic_endpoint_capacity = 0;

    AIRY_FREE(gw);
}

static const char *http2_gateway_get_name_impl(void *impl)
{
    (void)impl;
    return "HTTP/2 Gateway";
}

static airy_err_t http2_gateway_get_stats_impl(void *impl, char **out_json)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;
    if (!gw || !out_json)
        return AIRY_EINVAL;

#ifdef AIRY_HAS_CJSON
    cJSON *stats = cJSON_CreateObject();
    if (!stats)
        return AIRY_ENOMEM;

    cJSON_AddNumberToObject(stats, "requests_total", (double)atomic_load(&gw->base.requests_total));
    cJSON_AddNumberToObject(stats, "requests_failed",
                            (double)atomic_load(&gw->base.requests_failed));
    cJSON_AddNumberToObject(stats, "bytes_received", (double)atomic_load(&gw->base.bytes_received));
    cJSON_AddNumberToObject(stats, "bytes_sent", (double)atomic_load(&gw->base.bytes_sent));
    cJSON_AddNumberToObject(stats, "active_sessions", (double)gw->session_count);
    cJSON_AddNumberToObject(stats, "max_concurrent_streams", (double)gw->max_concurrent_streams);
    cJSON_AddStringToObject(stats, "protocol", "h2");

    char *json_str = cJSON_PrintUnformatted(stats);
    cJSON_Delete(stats);

    if (!json_str)
        return AIRY_ENOMEM;
    *out_json = json_str;
#else
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"requests_total\":%llu,\"requests_failed\":%llu,\"bytes_received\":%llu,"
             "\"bytes_sent\":%llu,\"active_sessions\":%zu,\"protocol\":\"h2\"}",
             (unsigned long long)atomic_load(&gw->base.requests_total),
             (unsigned long long)atomic_load(&gw->base.requests_failed),
             (unsigned long long)atomic_load(&gw->base.bytes_received),
             (unsigned long long)atomic_load(&gw->base.bytes_sent), gw->session_count);
    *out_json = AIRY_STRDUP(buf);
#endif

    return AIRY_SUCCESS;
}

static bool http2_gateway_is_running_impl(void *impl)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;
    if (!gw)
        return false;
    return atomic_load(&gw->running);
}

static airy_err_t http2_gateway_set_handler_impl(void *impl, gateway_internal_handler_t handler,
                                                 void *user_data)
{
    http2_gateway_t *gw = (http2_gateway_t *)impl;
    if (!gw)
        return AIRY_EINVAL;

    if (gw->base.handler_adapter) {
        AIRY_FREE(gw->base.handler_adapter);
        gw->base.handler_adapter = NULL;
    }

    gw->base.handler = handler;
    gw->base.handler_data = user_data;

    return AIRY_SUCCESS;
}

/**
  * @brief HTTP/2 gateway operations table
 */
static const gateway_ops_t http2_gateway_ops = {
    .start = http2_gateway_start_impl,
    .stop = http2_gateway_stop_impl,
    .destroy = http2_gateway_destroy_impl,
    .get_name = http2_gateway_get_name_impl,
    .get_stats = http2_gateway_get_stats_impl,
    .is_running = http2_gateway_is_running_impl,
    .set_handler = http2_gateway_set_handler_impl,
};

/**
  * @brief Initialize CORS config (read from environment variables)
 */
static void http2_init_cors_config(http_gateway_t *base)
{
    base->cors.allow_all_origins = false;
    base->cors.allowed_origins = NULL;
    base->cors.allowed_origins_count = 0;
    base->cors.allowed_methods = AIRY_STRDUP("POST, GET, OPTIONS");
    base->cors.allowed_headers = AIRY_STRDUP("Content-Type, Authorization");
    base->cors.max_age = 3600;

    const char *cors_mode = getenv("GATEWAY_CORS_MODE");
    if (cors_mode && strcmp(cors_mode, "dev") == 0) {
        base->cors.allow_all_origins = true;
    }

    const char *cors_origins = getenv("GATEWAY_CORS_ORIGINS");
    if (cors_origins && !base->cors.allow_all_origins) {
        char *origins_copy = AIRY_STRDUP(cors_origins);
        if (origins_copy) {
            size_t count = 1;
            for (char *p = origins_copy; *p; p++) {
                if (*p == ',')
                    count++;
            }

            if (count <= SIZE_MAX / sizeof(char *)) {
                base->cors.allowed_origins = (char **)airy_malloc_array(count, sizeof(char *));
                if (base->cors.allowed_origins) {
                    char *saveptr = NULL;
                    char *token = strtok_r(origins_copy, ",", &saveptr);
                    size_t i = 0;
                    while (token && i < count) {
                        base->cors.allowed_origins[i++] = AIRY_STRDUP(token);
                        token = strtok_r(NULL, ",", &saveptr);
                    }
                    base->cors.allowed_origins_count = i;
                }
            }
            AIRY_FREE(origins_copy);
        }
    }
}

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
    base->daemon = NULL;
    base->port = port;
    base->host = AIRY_STRDUP(host);
    base->handler_adapter = NULL;
    base->handler = NULL;
    base->handler_data = NULL;

    if (!base->host) {
        AIRY_FREE(gw);
        return NULL;
    }

    atomic_init(&base->running, false);
    atomic_init(&base->requests_total, 0);
    atomic_init(&base->requests_failed, 0);
    atomic_init(&base->bytes_received, 0);
    atomic_init(&base->bytes_sent, 0);

    base->max_request_size = 1 * 1024 * 1024;
    const char *env_max_size = getenv("GATEWAY_MAX_REQUEST_SIZE");
    if (env_max_size) {
        long size = strtol(env_max_size, NULL, 10);
        if (size > 0 && size <= 100 * 1024 * 1024) {
            base->max_request_size = (size_t)size;
        }
    }

    http2_init_cors_config(base);

    base->rate_limiter = NULL;
    const char *rate_limit_enabled = getenv("GATEWAY_RATE_LIMIT_ENABLED");
    if (rate_limit_enabled && strcmp(rate_limit_enabled, "true") == 0) {
        gateway_rate_limit_config_t rl_config;
        gateway_rate_limiter_get_default_config(&rl_config);
        rl_config.enabled = true;

        const char *rps = getenv("GATEWAY_RATE_LIMIT_RPS");
        if (rps) {
            rl_config.requests_per_second = (uint32_t)strtol(rps, NULL, 10);
        }

        base->rate_limiter = gateway_rate_limiter_create(&rl_config);
    }

    base->protocol_handler = gateway_protocol_handler_create(NULL);

    base->dynamic_endpoints = NULL;
    base->dynamic_endpoint_count = 0;
    base->dynamic_endpoint_capacity = 0;

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
        if (base->host)
            AIRY_FREE(base->host);
        if (base->cors.allowed_methods)
            AIRY_FREE(base->cors.allowed_methods);
        if (base->cors.allowed_headers)
            AIRY_FREE(base->cors.allowed_headers);
        if (base->cors.allowed_origins) {
            for (size_t i = 0; i < base->cors.allowed_origins_count; i++)
                AIRY_FREE(base->cors.allowed_origins[i]);
            AIRY_FREE(base->cors.allowed_origins);
        }
        if (base->protocol_handler)
            gateway_protocol_handler_destroy(base->protocol_handler);
        if (base->rate_limiter)
            gateway_rate_limiter_destroy(base->rate_limiter);
        AIRY_FREE(gw);
        return NULL;
    }

    gateway->ops = &http2_gateway_ops;
    gateway->impl = gw;
    gateway->type = GATEWAY_TYPE_HTTP;
    gateway->public_handler = NULL;
    gateway->public_handler_data = NULL;

    AIRY_LOG_INFO("HTTP/2 gateway created on %s:%u", host, port);

    return gateway;
}

int http2_gateway_start(http2_gateway_t *gw)
{
    if (!gw)
        return AIRY_EINVAL;
    return http2_gateway_start_impl(gw);
}

int http2_gateway_stop(http2_gateway_t *gw)
{
    if (!gw)
        return AIRY_EINVAL;
    http2_gateway_stop_impl(gw);
    return AIRY_SUCCESS;
}

#endif /* AIRY_HAS_HTTP2 */
#ifndef AIRY_HAS_HTTP2

gateway_t *http2_gateway_create(const char *host __attribute__((unused)),
                                uint16_t port __attribute__((unused)))
{
    AIRY_LOG_WARN("HTTP/2 gateway not available: nghttp2 not compiled in");
    return NULL;
}

int http2_gateway_start(http2_gateway_t *gw __attribute__((unused)))
{
    return AIRY_ENOSYS;
}

int http2_gateway_stop(http2_gateway_t *gw __attribute__((unused)))
{
    return AIRY_ENOSYS;
}

#endif /* !AIRY_HAS_HTTP2 */
