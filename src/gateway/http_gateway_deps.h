// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http_gateway_deps.h
 * @brief Shared dependency include block for the HTTP gateway route family.
 *
 * Single include point for the family-wide dependencies shared by the route
 * handlers (http_gateway_routes.c) and the SSE streaming subsystem
 * (http_gateway_sse_internal.h). PRIVATE to the gateway translation unit —
 * never install or expose it outside the gateway module.
 */

/* @owner: team-B */
#ifndef HTTP_GATEWAY_DEPS_H
#define HTTP_GATEWAY_DEPS_H

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

#endif /* HTTP_GATEWAY_DEPS_H */
