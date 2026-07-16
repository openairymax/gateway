// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
// @owner: team-B
#define GATEWAY_API_IMPLEMENTATION
#include "error.h"
#include "error.h"
#include "gateway_internal.h"
#include "http_gateway.h"
#include "logging.h"
#include "airy_memory.h"
#include "stdio_gateway.h"
#include "ws_gateway.h"

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

void gateway_destroy(gateway_t *gw)
{
    if (!gw)
        return;
    if (gw->ops && gw->ops->destroy) {
        gw->ops->destroy(gw->impl);
    }
    if (g_gateway_stats.running) {
        g_gateway_stats.running = false;
        g_gateway_stats.active_connections = 0;
    }
    AIRY_FREE(gw);
}

int gateway_start(gateway_t *gw)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    LOG_INFO("gateway_start: starting gateway (type=%d)", gw->type);
    int err = 0;
    if (gw->ops && gw->ops->start) {
        err = gw->ops->start(gw->impl);
    }
    if (err == 0) {
        g_gateway_stats.start_time = time(NULL);
        g_gateway_stats.running = true;
        LOG_INFO("gateway_start: gateway started successfully");
    } else {
        LOG_ERROR("gateway_start: start failed, err=%d", err);
    }
    return err;
}

int gateway_stop(gateway_t *gw)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    LOG_INFO("gateway_stop: stopping gateway (type=%d, uptime=%gs)",
                  gw->type, difftime(time(NULL), g_gateway_stats.start_time));
    if (gw->ops && gw->ops->stop) {
        gw->ops->stop(gw->impl);
    }
    g_gateway_stats.running = false;
    g_gateway_stats.active_connections = 0;
    LOG_INFO("gateway_stop: gateway stopped");
    return 0;
}

int gateway_get_stats(gateway_t *gw, char **out_json)
{
    AIRY_CHECK(gw != NULL, AIRY_ERR_NULL_POINTER, "gw is NULL");
    AIRY_CHECK(out_json != NULL, AIRY_ERR_NULL_POINTER, "out_json is NULL");

    double uptime_seconds = difftime(time(NULL), g_gateway_stats.start_time);

#ifdef AIRY_HAS_CJSON
    cJSON *stats = cJSON_CreateObject();
    cJSON_AddStringToObject(stats, "status", g_gateway_stats.running ? "running" : "stopped");
    cJSON_AddNumberToObject(stats, "uptime_seconds", uptime_seconds);
    cJSON_AddNumberToObject(stats, "total_connections", (double)g_gateway_stats.total_connections);
    cJSON_AddNumberToObject(stats, "active_connections",
                            (double)g_gateway_stats.active_connections);

    *out_json = cJSON_PrintUnformatted(stats);
    cJSON_Delete(stats);
#else
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"%s\",\"uptime_seconds\":%.1f,\"total_connections\":%llu,\"active_"
             "connections\":%llu}",
             g_gateway_stats.running ? "running" : "stopped", uptime_seconds,
             (unsigned long long)g_gateway_stats.total_connections,
             (unsigned long long)g_gateway_stats.active_connections);
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
