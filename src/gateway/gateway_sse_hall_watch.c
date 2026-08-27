// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_sse_hall_watch.c
 * @brief Hall event SSE push subscription (GET /api/v1/hall/watch).
 *
 * Extracted from http_gateway_sse.c — long-lived SSE subscription over the
 * hall event flow: every newly recorded hall event from any writer process
 * is pushed in global (ts_utc, seq) order. Real-time push counterpart of
 * hall.stream (which is poll-based pull).
 */

#include "http_gateway_sse_internal.h"
#include "gateway_hall_store.h"

typedef struct {
    gw_hall_watch_t watch;
    int done;
} gw_hall_watch_ctx_t;

static ssize_t gw_hall_watch_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    gw_hall_watch_ctx_t *w = (gw_hall_watch_ctx_t *)cls;
    if (!w || w->done)
        return MHD_CONTENT_READER_END_OF_STREAM;
    if (max < 8)
        return MHD_CONTENT_READER_END_OF_STREAM;

    char evt[8192];
    int r = gw_hall_watch_next(&w->watch, evt, sizeof(evt));
    if (r < 0) {
        w->done = 1;
        return MHD_CONTENT_READER_END_OF_STREAM;
    }
    if (r > 0) {
        size_t el = strlen(evt);
        if (el + 8 > max) {
            el = max - 8;
            evt[el] = '\0';
        }
        size_t n = 0;
        AIRY_MEMCPY(buf + n, "data: ", 6);
        n += 6;
        AIRY_MEMCPY(buf + n, evt, el);
        n += el;
        buf[n++] = '\n';
        buf[n++] = '\n';
        return (ssize_t)n;
    }

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
    nanosleep(&ts, NULL);
    AIRY_MEMCPY(buf, ": keep-alive\n\n", 15);
    return 15;
}

static void gw_hall_watch_free(void *cls)
{
    gw_hall_watch_ctx_t *w = (gw_hall_watch_ctx_t *)cls;
    AIRY_FREE(w);
}

int handle_hall_watch_sse(http_gateway_t *gateway, struct MHD_Connection *connection,
                          http_request_context_t *context)
{
    (void)context;
    gw_hall_watch_ctx_t *w = (gw_hall_watch_ctx_t *)AIRY_CALLOC(1, sizeof(*w));
    if (!w)
        return gw_sse_send_json_error(gateway, connection, 500, "Out of memory");
    gw_hall_watch_init(&w->watch);

    struct MHD_Response *response =
        MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, GW_SSE_BLOCK_SIZE,
                                          gw_hall_watch_reader, w, gw_hall_watch_free);
    if (!response) {
        AIRY_FREE(w);
        return gw_sse_send_json_error(gateway, connection, 500, "Failed to create stream response");
    }
    MHD_add_response_header(response, "Content-Type", "text/event-stream");
    MHD_add_response_header(response, "Cache-Control", "no-cache");
    MHD_add_response_header(response, "Connection", "keep-alive");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    gateway_apply_cors_headers(gateway, connection, response);

    atomic_fetch_add(&gateway->requests_total, 1);
    int ret = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    return ret;
}
