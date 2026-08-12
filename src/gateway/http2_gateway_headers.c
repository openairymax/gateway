// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http2_gateway_headers.c
 * @brief HTTP/2 gateway response header domain (nghttp2_nv header building and release).
 */

// @owner: team-B
#define LOG_TAG "http2_gateway"
#include "http2_gateway.h"
#include "http2_gateway_internal.h"

#ifdef AIRY_HAS_HTTP2

/**
  * @brief Build HTTP/2 response headers (nghttp2_nv array)
 *
 * Includes the :status pseudo-header, content-type, security and CORS headers.
 *
 * @param ctx Stream context (used to read origin)
 * @param gw Gateway instance (for CORS config)
 * @param nva Output header array (caller-allocated)
 * @param max_nva Maximum array capacity
 * @return Number of headers actually filled
 */
size_t http2_build_response_headers(http2_stream_context_t *ctx, http2_gateway_t *gw,
                                    nghttp2_nv *nva, size_t max_nva)
{
    size_t count = 0;
    char status_str[8];
    http_gateway_t *base = &gw->base;

    snprintf(status_str, sizeof(status_str), "%d", ctx->response_status);
    if (count < max_nva) {
        nva[count].name = (uint8_t *)":status";
        nva[count].namelen = 7;
        nva[count].value = (uint8_t *)AIRY_STRDUP(status_str);
        if (!nva[count].value) {

            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = strlen(status_str);
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }

    /* content-type */
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"content-type";
        nva[count].namelen = 12;
        nva[count].value = (uint8_t *)AIRY_STRDUP("application/json");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 16;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }

    /* server */
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"server";
        nva[count].namelen = 6;
        nva[count].value = (uint8_t *)AIRY_STRDUP("AgentRT-gateway/2.0");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 19;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }

    if (count < max_nva) {
        nva[count].name = (uint8_t *)"x-content-type-options";
        nva[count].namelen = 22;
        nva[count].value = (uint8_t *)AIRY_STRDUP("nosniff");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 7;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"x-frame-options";
        nva[count].namelen = 15;
        nva[count].value = (uint8_t *)AIRY_STRDUP("DENY");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 4;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"strict-transport-security";
        nva[count].namelen = 25;
        nva[count].value = (uint8_t *)AIRY_STRDUP("max-age=31536000; includeSubDomains");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 35;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }
    if (count < max_nva) {
        nva[count].name = (uint8_t *)"cache-control";
        nva[count].namelen = 13;
        nva[count].value = (uint8_t *)AIRY_STRDUP("no-store, no-cache, must-revalidate");
        if (!nva[count].value) {
            http2_free_response_headers(nva, count);
            return (size_t)-1;
        }
        nva[count].valuelen = 34;
        nva[count].flags = NGHTTP2_NV_FLAG_NONE;
        count++;
    }

    if (ctx->origin && base) {
        bool origin_allowed = false;
        if (base->cors.allow_all_origins) {
            origin_allowed = true;
        } else {
            for (size_t i = 0; i < base->cors.allowed_origins_count; i++) {
                if (base->cors.allowed_origins[i] &&
                    strcmp(ctx->origin, base->cors.allowed_origins[i]) == 0) {
                    origin_allowed = true;
                    break;
                }
            }
        }

        if (origin_allowed && count < max_nva) {
            nva[count].name = (uint8_t *)"access-control-allow-origin";
            nva[count].namelen = 27;
            nva[count].value = (uint8_t *)AIRY_STRDUP(ctx->origin);
            if (!nva[count].value) {
                http2_free_response_headers(nva, count);
                return (size_t)-1;
            }
            nva[count].valuelen = strlen(ctx->origin);
            nva[count].flags = NGHTTP2_NV_FLAG_NONE;
            count++;
        }
    }

    return count;
}

/**
  * @brief Free the dynamically allocated value strings in a response header array
 */
void http2_free_response_headers(nghttp2_nv *nva, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        AIRY_FREE(nva[i].value);
    }
}

#endif /* AIRY_HAS_HTTP2 */
