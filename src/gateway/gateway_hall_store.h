/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_hall_store.h
 * @brief Gateway-side hall event recording (write side of the event flow).
 *
 * The runtime writes the single-source-of-truth event flow (hall_store,
 * atoms/coreloopthree) into $AIRY_DATA_DIR/agentrt/hall, and the gateway
 * already implements the read side (gateway_biz_hall.c: hall.board/tasks/
 * replay/stream). This module adds the counterpart write side so that
 * sessions that never touch the in-process hall handle - SSE streaming
 * chat (handle_chat_stream_sse) and gateway-orchestrated agent.run - still
 * produce visible events (decision chain / board), keeping the invariant
 * "what a client can see is what has been recorded" across all entry
 * points.
 *
 * Event file format is byte-for-byte aligned with hall_store.c:
 *   root:  $AIRY_DATA_DIR/agentrt/hall
 *   file:  {tenant}.{task}.{category}.{ts_utc}.{seq:04u}.json
 *   body:  {"file":{...},"access":{...},"content":{...}}
 * Cross-process order is (ts_utc, seq) like the read side; gseq is a
 * per-process monotonic counter (it restarts with each process and is
 * only used for in-process audit, never for cross-process ordering).
 */

#ifndef AIRY_RT_GATEWAY_HALL_STORE_H
#define AIRY_RT_GATEWAY_HALL_STORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record one hall event (task-file model).
 * @param task_id      task ID (e.g. "preflight", "exec-<id>", "gw-<ts>")
 * @param category     category name ("blueprint"/"command"/"progress"/
 *                     "result"/"issue"/"verify"/"chain")
 * @param node_id      blueprint node ID, or NULL
 * @param content_json JSON object string for the content section (caller
 *                     guarantees it parses as a JSON object)
 * @return 0 on success, non-zero on failure (never fails the caller's flow)
 */
int gw_hall_store_event(const char *task_id, const char *category, const char *node_id,
                        const char *content_json);

/**
 * @brief Generate a gateway session task ID ("gw-" + UTC timestamp), e.g.
 *        "gw-20260816T120000000". Used for SSE chat sessions that carry no
 *        client-supplied session_id.
 */
void gw_hall_task_id_now(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_HALL_STORE_H */
