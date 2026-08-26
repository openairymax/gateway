// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
#define GATEWAY_API_IMPLEMENTATION
#include "error.h"
#include "gateway_internal.h"
#include "http_gateway.h"
#include "logging.h"
#include "airy_memory.h"
#include "stdio_gateway.h"
#include "ws_gateway.h"

#include "atomic_compat.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

gateway_t *gateway_http_create(const char *host, uint16_t port)
{
    return http_gateway_create(host, port);
}

gateway_t *gateway_ws_create(const char *host, uint16_t port)
{
    return ws_gateway_create(host, port);
}

gateway_t *gateway_stdio_create(void)
{
    return stdio_gateway_create();
}

static struct {
    uint64_t total_connections;
    uint64_t active_connections;
    time_t start_time;
    bool running;
} g_gateway_stats = {0, 0, 0, false};

/* 全局统计跨实例/线程共享：CAS 惰性初始化互斥锁保护（0=未初始化，
 * 2=初始化中，1=就绪），与 gateway_hall_store 同范式。 */
static atomic_int g_gw_stats_ready = 0;
static airy_mtx_t g_gw_stats_lock;

static void gw_stats_ensure_init(void)
{
    while (atomic_load_explicit(&g_gw_stats_ready, memory_order_acquire) != 1) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_gw_stats_ready, &expected, 2,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            airy_mtx_init(&g_gw_stats_lock);
            atomic_store_explicit(&g_gw_stats_ready, 1, memory_order_release);
            break;
        }
    }
}

void gateway_destroy(gateway_t *gw)
{
    if (!gw)
        return;
    if (gw->ops && gw->ops->destroy) {
        gw->ops->destroy(gw->impl);
    }
    gw_stats_ensure_init();
    airy_mtx_lock(&g_gw_stats_lock);
    g_gateway_stats.running = false;
    g_gateway_stats.active_connections = 0;
    airy_mtx_unlock(&g_gw_stats_lock);
    AIRY_FREE(gw);
}

int gateway_start(gateway_t *gw)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    AIRY_LOG_INFO("gateway_start: starting gateway (type=%d)", gw->type);
    int err = 0;
    if (gw->ops && gw->ops->start) {
        err = gw->ops->start(gw->impl);
    }
    if (err == 0) {
        gw_stats_ensure_init();
        airy_mtx_lock(&g_gw_stats_lock);
        g_gateway_stats.start_time = time(NULL);
        g_gateway_stats.running = true;
        airy_mtx_unlock(&g_gw_stats_lock);
        AIRY_LOG_INFO("gateway_start: gateway started successfully");
    } else {
        AIRY_LOG_ERROR("gateway_start: start failed, err=%d", err);
    }
    return err;
}

int gateway_stop(gateway_t *gw)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    gw_stats_ensure_init();
    airy_mtx_lock(&g_gw_stats_lock);
    double uptime_seconds = difftime(time(NULL), g_gateway_stats.start_time);
    airy_mtx_unlock(&g_gw_stats_lock);
    AIRY_LOG_INFO("gateway_stop: stopping gateway (type=%d, uptime=%gs)", gw->type,
             uptime_seconds);
    if (gw->ops && gw->ops->stop) {
        gw->ops->stop(gw->impl);
    }
    airy_mtx_lock(&g_gw_stats_lock);
    g_gateway_stats.running = false;
    g_gateway_stats.active_connections = 0;
    airy_mtx_unlock(&g_gw_stats_lock);
    AIRY_LOG_INFO("gateway_stop: gateway stopped");
    return 0;
}

int gateway_get_stats(gateway_t *gw, char **out_json)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    AIRY_CHECK(out_json != NULL, AIRY_ERR_NULL_POINTER, "out_json is NULL");

    gw_stats_ensure_init();
    airy_mtx_lock(&g_gw_stats_lock);
    double uptime_seconds = difftime(time(NULL), g_gateway_stats.start_time);
    bool running = g_gateway_stats.running;
    uint64_t total_conn = g_gateway_stats.total_connections;
    uint64_t active_conn = g_gateway_stats.active_connections;
    airy_mtx_unlock(&g_gw_stats_lock);

#ifdef AIRY_HAS_CJSON
    cJSON *stats = cJSON_CreateObject();
    cJSON_AddStringToObject(stats, "status", running ? "running" : "stopped");
    cJSON_AddNumberToObject(stats, "uptime_seconds", uptime_seconds);
    cJSON_AddNumberToObject(stats, "total_connections", (double)total_conn);
    cJSON_AddNumberToObject(stats, "active_connections", (double)active_conn);

    *out_json = cJSON_PrintUnformatted(stats);
    cJSON_Delete(stats);
#else
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"%s\",\"uptime_seconds\":%.1f,\"total_connections\":%llu,\"active_"
             "connections\":%llu}",
             running ? "running" : "stopped", uptime_seconds,
             (unsigned long long)total_conn, (unsigned long long)active_conn);
    *out_json = AIRY_STRDUP(buf);
#endif
    return 0;
}

int gateway_set_handler(gateway_t *gw, gateway_request_handler_t handler, void *user_data)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    gw->public_handler = handler;
    gw->public_handler_data = user_data;
    return 0;
}

bool gateway_is_running(gateway_t *gw)
{
    if (!gw || !gw->ops || !gw->ops->is_running)
        return false;
    return gw->ops->is_running(gw->impl);
}

gateway_type_t gateway_get_type(gateway_t *gw)
{
    if (!gw)
        return GATEWAY_TYPE_HTTP;
    return gw->type;
}

const char *gateway_get_name(gateway_t *gw)
{
    if (!gw || !gw->ops || !gw->ops->get_name)
        return "unknown";
    return gw->ops->get_name(gw->impl);
}

int gateway_register_endpoint(gateway_t *gw, const char *method, const char *path,
                              gateway_endpoint_handler_t handler, void *user_data)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    AIRY_CHECK(method != NULL, AIRY_ERR_NULL_POINTER, "method is NULL");
    AIRY_CHECK(path != NULL, AIRY_ERR_NULL_POINTER, "path is NULL");
    AIRY_CHECK(handler != NULL, AIRY_ERR_NULL_POINTER, "handler is NULL");
    AIRY_CHECK(gw->type == GATEWAY_TYPE_HTTP, AIRY_ERR_INVALID_PARAM, "gw type is not HTTP");
    return http_gateway_register_endpoint((http_gateway_t *)gw->impl, method, path, handler,
                                          user_data);
}
