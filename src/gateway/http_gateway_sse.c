// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file http_gateway_sse.c
 * @brief Shared SSE JSON-error responder for the gateway HTTP streaming
 *        subsystem.
 *
 * History: this translation unit formerly hosted the legacy
 * /api/v1/chat/stream tool-loop content_reader state machine
 * (LLM_ROUND/LLM_STREAM/EXEC_TOOLS/REASONING/FINAL_TEXT) plus its socket
 * recv and frame helpers. That endpoint was retired (410 Gone) and the
 * dead orchestration body — together with the frame/stream/tool/memory
 * helper modules — was physically removed.
 *
 * Only the shared gw_sse_send_json_error() responder remains; it is used by
 * the live streaming endpoints (gateway_sse_run_stream.c and
 * gateway_sse_hall_watch.c).
 */

#include "http_gateway_sse_internal.h"

/* ── JSON error response (shared with the live SSE endpoints) ──────── */

int gw_sse_send_json_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                           int status, const char *message)
{
    char err[256];
    int n = snprintf(err, sizeof(err), "{\"error\":{\"code\":%d,\"message\":\"%s\"}}", status,
                     message ? message : "error");
    if (n < 0 || n >= (int)sizeof(err)) {
        AIRY_STRNCPY_TERM(err, "{\"error\":{\"code\":500,\"message\":\"error\"}}", sizeof(err));
    }
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, status, err, strlen(err));
    int ret = MHD_NO;
    if (response) {
        ret = MHD_queue_response(connection, status, response);
        MHD_destroy_response(response);
    }
    atomic_fetch_add(&gateway->requests_failed, 1);
    return ret;
}

/* ── 503 responder for a restarting southbound service ─────────────── */

int gw_sse_send_unavail(http_gateway_t *gateway, struct MHD_Connection *connection,
                        int retry_ms)
{
    if (retry_ms < 0)
        retry_ms = 0;
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32000,"
                     "\"message\":\"agent service restarting\","
                     "\"data\":{\"code\":\"UNAVAILABLE\",\"retry_after_ms\":%d}}}",
                     retry_ms);
    if (n < 0 || n >= (int)sizeof(body)) {
        AIRY_STRNCPY_TERM(body,
                          "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32000,"
                          "\"message\":\"agent service restarting\","
                          "\"data\":{\"code\":\"UNAVAILABLE\"}}}",
                          sizeof(body));
    }
    struct MHD_Response *response =
        create_http_response_ex(gateway, connection, 503, body, strlen(body));
    int ret = MHD_NO;
    if (response) {
        char retry_s[16];
        snprintf(retry_s, sizeof(retry_s), "%d", (retry_ms + 999) / 1000);
        MHD_add_response_header(response, "Retry-After", retry_s);
        ret = MHD_queue_response(connection, 503, response);
        MHD_destroy_response(response);
    }
    atomic_fetch_add(&gateway->requests_failed, 1);
    return ret;
}
