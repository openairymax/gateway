// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http_gateway_sse_internal.h
 * @brief Shared includes and declarations for the gateway SSE streaming
 *        subsystem.
 *
 * The subsystem consists of three translation units:
 *   http_gateway_sse.c        — shared gw_sse_send_json_error() responder
 *   gateway_sse_hall_watch.c  — long-lived SSE subscription over hall store
 *   gateway_sse_run_stream.c  — agent.run_stream event-frame translator
 *
 * The legacy /api/v1/chat/stream orchestration modules (frame/stream/tool/
 * memory) were physically removed; the endpoint was retired (410 Gone).
 *
 * This header is PRIVATE to the SSE subsystem — never install or expose it
 * outside the gateway translation unit.
 */

#ifndef HTTP_GATEWAY_SSE_INTERNAL_H
#define HTTP_GATEWAY_SSE_INTERNAL_H

#include "http_gateway_routes.h"

#include "gateway_rate_limiter.h"
#include "gateway_rpc_handler.h"
#include "gateway_utils.h"
#include "http_gateway.h"
#include "jsonrpc.h"
#include "logging.h"
#include "airy_memory.h"
#include "platform.h"
#include "syscall_router.h"
#include "syscall_router_internal.h"
#include "syscalls.h"

#include <microhttpd.h>
#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "atomic_compat.h"

/* ── Constants ─────────────────────────────────────────────────────── */

/* MHD response block size for the long-lived SSE subscription endpoint. */
#define GW_SSE_BLOCK_SIZE      1024

/* ── Shared helpers (defined in http_gateway_sse.c) ────────────────── */

int gw_sse_send_json_error(http_gateway_t *gateway, struct MHD_Connection *connection,
                           int status, const char *message);

#endif /* HTTP_GATEWAY_SSE_INTERNAL_H */
