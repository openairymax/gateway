// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file ws_gateway.c
 * @brief WebSocket gateway - lifecycle, ops and creation.
 *
 * Implements the WebSocket gateway instance lifecycle: context/thread
 * management (start/stop/destroy), the gateway ops table (name/stats/
 * running/handler), creation and the non-WS fallback stub. Message
 * encoding/handling and event callback dispatch live in
 * ws_gateway_message.c / ws_gateway_callback.c.
 *
 * Design principles:
 *   K-1 minimal core: only protocol translation, zero business logic
 *   S-2 layered decomposition: single responsibility per layer
 *   E-8 testability: route handlers independently testable
 */

// @owner: team-B
#include "ws_gateway.h"

#include "ws_gateway_internal.h"

#include "gateway_rate_limiter.h"

#include "airy_memory.h"
#include "error.h"
#include "logging.h"

#ifdef GATEWAY_HAS_WS

#include <cjson/cJSON.h>

#include <stdlib.h>
#include <string.h>

#include "atomic_compat.h"

#ifndef _WIN32
#include <pthread.h>
#endif

#ifndef _WIN32
/**
  * @brief libwebsockets event-loop thread
  *
  * lws_create_context only creates the context; lws_service must be called
  * to drive I/O (IRON-2: a real WebSocket server, not a stub).
  * 50ms timeout lets the thread exit and join promptly after running=false.
 */
static void *ws_gateway_event_loop(void *arg)
{
    ws_gateway_t *gateway = (ws_gateway_t *)arg;
    while (gateway && atomic_load(&gateway->running)) {
        lws_service(gateway->context, 50);
    }
    return NULL;
}
#endif

static airy_err_t ws_gateway_start(void *gateway_impl)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;

    struct lws_context_creation_info info;
    AIRY_MEMSET(&info, 0, sizeof(info));
    info.port = gateway->port;
    info.iface = gateway->host;
    info.protocols = ws_protocols;
    info.user = gateway;

    gateway->context = lws_create_context(&info);
    if (!gateway->context) {
        return AIRY_EBUSY;
    }

    atomic_store(&gateway->running, true);

#ifndef _WIN32

    pthread_t *thread = (pthread_t *)AIRY_MALLOC(sizeof(pthread_t));
    if (!thread) {
        atomic_store(&gateway->running, false);
        lws_context_destroy(gateway->context);
        gateway->context = NULL;
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    if (pthread_create(thread, NULL, ws_gateway_event_loop, gateway) != 0) {
        AIRY_FREE(thread);
        atomic_store(&gateway->running, false);
        lws_context_destroy(gateway->context);
        gateway->context = NULL;
        return AIRY_EBUSY;
    }
    gateway->event_thread = thread;
#else

    gateway->event_thread = NULL;
#endif

    return AIRY_SUCCESS;
}

static void ws_gateway_stop(void *gateway_impl)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;

    atomic_store(&gateway->running, false);

#ifndef _WIN32
    if (gateway->event_thread) {
        /* lws_cancel_service safely wakes the lws_service loop from other threads,
          * so the event thread exits right after running=false. Otherwise the loop
          * waits for the next 50ms poll timeout; under netlink event storms,
          * lws_service may process events for long, blocking join for seconds,
          * and the graceful exit exceeds the external stop threshold, getting KILLed. */
        if (gateway->context) {
            lws_cancel_service(gateway->context);
        }
        pthread_join(*(pthread_t *)gateway->event_thread, NULL);
        AIRY_FREE(gateway->event_thread);
        gateway->event_thread = NULL;
    }
#endif

    if (gateway->context) {
        lws_context_destroy(gateway->context);
        gateway->context = NULL;
    }
}

static void ws_gateway_destroy(void *gateway_impl)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;

    ws_gateway_stop(gateway);

    if (gateway->handler_adapter) {
        AIRY_FREE(gateway->handler_adapter);
        gateway->handler_adapter = NULL;
    }
    gateway->handler = NULL;
    gateway->handler_data = NULL;

    if (gateway->host) {
        AIRY_FREE(gateway->host);
    }

    if (gateway->rate_limiter) {
        gateway_rate_limiter_destroy(gateway->rate_limiter);
        gateway->rate_limiter = NULL;
    }

    AIRY_FREE(gateway);
}

static const char *ws_gateway_get_name(void *gateway_impl __attribute__((unused)))
{
    return "WebSocket Gateway";
}

static bool ws_gateway_is_running(void *gateway_impl)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;
    if (!gateway)
        return false;
    return atomic_load(&gateway->running);
}

static airy_err_t ws_gateway_get_stats(void *gateway_impl, char **out_json)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;
    if (!gateway || !out_json)
        return AIRY_EINVAL;

    cJSON *stats = cJSON_CreateObject();
    if (!stats)
        return AIRY_ENOMEM;

    cJSON_AddNumberToObject(stats, "connections_total",
                            (double)atomic_load(&gateway->connections_total));
    cJSON_AddNumberToObject(stats, "connections_active",
                            (double)atomic_load(&gateway->connections_active));
    cJSON_AddNumberToObject(stats, "messages_total", (double)atomic_load(&gateway->messages_total));
    cJSON_AddNumberToObject(stats, "bytes_sent", (double)atomic_load(&gateway->bytes_sent));
    cJSON_AddNumberToObject(stats, "bytes_received", (double)atomic_load(&gateway->bytes_received));

    char *json_str = cJSON_Print(stats);
    cJSON_Delete(stats);

    if (!json_str)
        return AIRY_ENOMEM;

    *out_json = json_str;
    return AIRY_SUCCESS;
}

static airy_err_t ws_gateway_set_handler(void *gateway_impl, gateway_internal_handler_t handler,
                                         void *user_data)
{
    ws_gateway_t *gateway = (ws_gateway_t *)gateway_impl;
    if (!gateway)
        return AIRY_EINVAL;

    if (gateway->handler_adapter) {
        AIRY_FREE(gateway->handler_adapter);
        gateway->handler_adapter = NULL;
    }

    gateway->handler = handler;
    gateway->handler_data = user_data;

    return AIRY_SUCCESS;
}

static const gateway_ops_t ws_gateway_ops = {.start = ws_gateway_start,
                                             .stop = ws_gateway_stop,
                                             .destroy = ws_gateway_destroy,
                                             .get_name = ws_gateway_get_name,
                                             .get_stats = ws_gateway_get_stats,
                                             .is_running = ws_gateway_is_running,
                                             .set_handler = ws_gateway_set_handler};

/**
  * @brief Create a WebSocket gateway instance
  * @param host Listen address (e.g. "127.0.0.1", "0.0.0.0"); must not be NULL
  * @param port Listen port (e.g. 8081)
  * @return Gateway handle, or NULL on failure (OOM or invalid args)
  *
 * @ownership Caller must release via gateway_destroy()
 * @threadsafe yes
 * @since 1.0.0
 */
gateway_t *ws_gateway_create(const char *host, uint16_t port)
{
    if (!host) {
        return NULL;
    }

    ws_gateway_t *gateway = AIRY_CALLOC(1, sizeof(ws_gateway_t));
    if (!gateway) {
        return NULL;
    }

    gateway->port = port;
    gateway->host = AIRY_STRDUP(host);
    gateway->handler_adapter = NULL;
    gateway->handler = NULL;
    gateway->handler_data = NULL;

    /* Rate limiting is opt-in, driven by the same env vars as the HTTP/2
     * gateway (GATEWAY_RATE_LIMIT_ENABLED=true [+ GATEWAY_RATE_LIMIT_RPS]). */
    gateway->rate_limiter = NULL;
    const char *rate_limit_enabled = getenv("GATEWAY_RATE_LIMIT_ENABLED");
    if (rate_limit_enabled && strcmp(rate_limit_enabled, "true") == 0) {
        gateway_rate_limit_config_t rl_config;
        gateway_rate_limiter_get_default_config(&rl_config);
        rl_config.enabled = true;
        const char *rps = getenv("GATEWAY_RATE_LIMIT_RPS");
        if (rps) {
            long v = strtol(rps, NULL, 10);
            if (v > 0 && v <= 100000) {
                rl_config.requests_per_second = (uint32_t)v;
            } else {
                AIRY_LOG_WARN("ignoring invalid GATEWAY_RATE_LIMIT_RPS: %s", rps);
            }
        }
        gateway->rate_limiter = gateway_rate_limiter_create(&rl_config);
        AIRY_LOG_INFO("WebSocket rate limiting enabled (rps=%u)",
                 rl_config.requests_per_second);
    }

    if (!gateway->host) {
        if (gateway->rate_limiter) {
            gateway_rate_limiter_destroy(gateway->rate_limiter);
        }
        AIRY_FREE(gateway);
        return NULL;
    }

    atomic_init(&gateway->running, false);
    atomic_init(&gateway->connections_total, 0);
    atomic_init(&gateway->connections_active, 0);
    atomic_init(&gateway->messages_total, 0);
    atomic_init(&gateway->bytes_sent, 0);
    atomic_init(&gateway->bytes_received, 0);

    gateway->max_request_size = 10 * 1024 * 1024; /* 10MB */
    gateway_t *gw = AIRY_MALLOC(sizeof(gateway_t));
    if (!gw) {
        if (gateway->rate_limiter) {
            gateway_rate_limiter_destroy(gateway->rate_limiter);
        }
        AIRY_FREE(gateway->host);
        AIRY_FREE(gateway);
        return NULL;
    }

    gw->ops = &ws_gateway_ops;
    gw->impl = gateway;
    gw->type = GATEWAY_TYPE_WS;
    gw->public_handler = NULL;
    gw->public_handler_data = NULL;

    return gw;
}

#endif /* GATEWAY_HAS_WS */
#ifndef GATEWAY_HAS_WS

gateway_t *ws_gateway_create(const char *host __attribute__((unused)),
                             uint16_t port __attribute__((unused)))
{
    return NULL;
}

#endif /* !GATEWAY_HAS_WS */
