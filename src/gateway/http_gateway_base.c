// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file http_gateway_base.c
 * @brief HTTP 家族（HTTP/1.1 与 HTTP/2）共用基类生命周期。
 *
 * http_gateway_t 是 HTTP/1.1（libmicrohttpd）与 HTTP/2（nghttp2）两
 * 条传输的公共基类（http2_gateway_t.base 为首成员）。此前两条传输各自
 * 复制了一份「atomic 计数器 + max_request_size 环境装配 + CORS 配置
 * 装配 + 限流器装配」与「host/CORS/限流器/协议处理器释放」，属机制层
 * 重复；本文件为唯一权威实现（SSoT）。
 *
 * 传输相关部分（监听套接字、事件循环、会话表）仍留在各自传输内。
 * protocol_handler 装配策略不同：HTTP/1.1 经 GATEWAY_PROTOCOL_HANDLER
 * 按需启用，HTTP/2 无条件启用，故不并入基数装配。
 */

// @owner: team-B
#include "http_gateway.h"

#include "gateway_protocol_handler.h"
#include "gateway_rate_limiter.h"
#include "airy_memory.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* max_request_size：默认 1MB，GATEWAY_MAX_REQUEST_SIZE 覆盖（上限 100MB）。 */
static void http_base_max_init(http_gateway_t *base)
{
    base->max_request_size = 1 * 1024 * 1024;
    const char *env_max_size = getenv("GATEWAY_MAX_REQUEST_SIZE");
    if (env_max_size) {
        long size = strtol(env_max_size, NULL, 10);
        if (size > 0 && size <= 100 * 1024 * 1024) {
            base->max_request_size = (size_t)size;
        }
    }
}

/* CORS：方法/头为静态默认串；来源列表由 GATEWAY_CORS_ORIGINS 装配。 */
static void http_base_cors_init(http_gateway_t *base)
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

bool http_base_init(http_gateway_t *base, const char *host, uint16_t port)
{
    base->port = port;
    base->host = AIRY_STRDUP(host);
    if (!base->host) {
        return false;
    }

    atomic_init(&base->running, false);
    atomic_init(&base->requests_total, 0);
    atomic_init(&base->requests_failed, 0);
    atomic_init(&base->bytes_received, 0);
    atomic_init(&base->bytes_sent, 0);
    base->epoch_s = (uint64_t)time(NULL);

    http_base_max_init(base);
    http_base_cors_init(base);
    base->rate_limiter = gw_rate_from_env();

    return true;
}

void http_base_destroy(http_gateway_t *base)
{
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
    }
}