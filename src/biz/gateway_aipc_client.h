/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
/**
 * @file gateway_aipc_client.h
 * @brief Gateway southbound A-IPC unified client face.
 *
 * Gateway has exactly ONE southbound client face toward the daemon plane.
 * Every socket(AF_UNIX) creation under gateway/src is funneled here; the
 * per-site handwritten UDS clients (sse_tool / sse_stream / sse_run_stream /
 * pep_cache) were deleted and now consume this face.
 *
 * Wire contract:
 *   - gw_aipc_call      REQUEST/RESPONSE: L2-first (channel_for_socket +
 *     daemon_l2_rpc_call_resp, blueprint 8.3.3 grey norm) with the UDS/TCP
 *     fallback path; misses fail fast, never silently downgrade.
 *   - gw_aipc_stream    STREAM: connect + send request, returns the fd; the
 *     caller owns the chunked read loop (MHD pull model keeps idle-deadline
 *     supervision). L2 STREAM client mapping is pending;
 *     today this is the fallback transport, centralized.
 *   - gw_aipc_subscribe EVENT: connect + send the subscription handshake,
 *     returns the fd; the caller owns the frame loop and reconnect policy.
 *
 * Gate N2 (verify_release_gates.sh): AF_UNIX stream-socket creation is
 * allowed ONLY in gateway_aipc_client.c under gateway/src.
 */

#ifndef AIRY_RT_GATEWAY_AIPC_CLIENT_H
#define AIRY_RT_GATEWAY_AIPC_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Full-response cap: oversized daemon replies fail the call (DoS guard). */
#define GW_AIPC_MAX_RESP 1048576

/**
 * @brief REQUEST/RESPONSE round trip against a daemon endpoint.
 *
 * Builds {"jsonrpc":"2.0","method":<method>,"params":<params_json>,"id":1},
 * sends it over the L2 channel when the transport switch resolves to
 * "corekern", otherwise over the socket/TCP fallback, and blocks until the
 * full JSON response is read (EOF-delimited).
 *
 * @param sock_path   Target endpoint: UDS path (POSIX) or "host:port" (WIN32
 *                    daemon convention: TCP loopback)
 * @param method      Internal service method (e.g. "spawn"/"invoke"/"write")
 * @param params_json Method params JSON string (NULL/empty -> "{}")
 * @param timeout_ms  Receive timeout (ms; WIN32 falls back to the LLM
 *                    default when non-positive)
 * @return Response JSON string (AIRY_MALLOC, caller AIRY_FREE), or NULL on
 *         failure
 */
char *gw_aipc_call(const char *sock_path, const char *method,
                   const char *params_json, int timeout_ms);

/**
 * @brief STREAM open: connect and ship the request bytes.
 *
 * Transition form (design §4.2): returns the connected fd; the caller keeps
 * its chunked read loop. @p poll_timeout_s > 0 installs SO_RCVTIMEO so a
 * stalled stream surfaces as EAGAIN to the caller's poll logic; 0 keeps the
 * fd blocking (readers that rely on blocking recv, e.g. run_stream).
 *
 * @param sock_path      Target endpoint (UDS path, POSIX only)
 * @param req_json       Request bytes sent verbatim (no NUL appended)
 * @param poll_timeout_s Optional SO_RCVTIMEO seconds (0 = blocking)
 * @return Connected fd on success, -1 on failure (WIN32: streaming paths are
 *         POSIX-only, callers already degrade)
 * @ownership caller closes the fd
 */
int gw_aipc_stream(const char *sock_path, const char *req_json,
                   int poll_timeout_s);

/**
 * @brief EVENT subscribe open: connect and ship the handshake bytes.
 *
 * Transition form (design §4.2): the PEP epoch watch (and any future event
 * consumer) hands in its subscription handshake (e.g. the notify_d HTTP SSE
 * request); this face owns connect + send. Frame parsing and the reconnect
 * policy stay with the caller (fail-open observer semantics).
 *
 * @param sock_path     Target endpoint (UDS path, POSIX only)
 * @param handshake     Handshake bytes sent verbatim after connect
 * @param handshake_len Handshake byte count
 * @return Connected fd on success, -1 on failure
 * @ownership caller closes the fd
 */
int gw_aipc_subscribe(const char *sock_path, const char *handshake,
                      size_t handshake_len);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_AIPC_CLIENT_H */
