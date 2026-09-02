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
 * Each event's header prev_file carries the file id of the previous event
 * in the same (task, category) dir ("" for the first one), so the decision
 * chain is reconstructible from the on-disk event flow alone.
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

/**
 * @brief Read a hall event file and flatten its envelope into the compact
 *        event JSON consumed by every hall.* read endpoint.
 *
 * Wire event bodies are stored with a {"file":{...},"access":{...},
 * "content":{...}} envelope (byte-aligned with the runtime writer). All
 * read surfaces (hall.replay / hall.stream events, hall.watch SSE push)
 * expose the flattened form {file_id,category,task_id,tenant_id,node_id,
 * ts_utc,seq,gseq,content} — a single on-the-wire event shape (SSoT), so
 * any client decodes stream and watch payloads identically.
 *
 * @param path  hall event file path
 * @return malloc'd compact JSON (caller AIRY_FREE); NULL on parse failure
 */
char *hall_event_flatten(const char *path);

/**
 * @brief Hall event watch cursor (read side, for SSE push).
 *
 * The gateway has no in-process hall handle (unlike the CLI/TUI which link
 * airy_coreloopthree); it reads the on-disk event flow. This cursor tracks
 * the last pushed event position so a long-lived SSE subscription can poll
 * the hall root and emit only newly recorded events, in global
 * (ts_utc, seq) order.
 */
typedef struct {
    char root[1024];   /* hall root (airy_data_dir()/agentrt/hall) */
    char last_ts[24];  /* last pushed ts_utc (fixed width, lexical) */
    unsigned long last_seq;
    int initialized;
} gw_hall_watch_t;

/**
 * @brief Initialize a hall watch cursor (resolves the hall root).
 */
void gw_hall_watch_init(gw_hall_watch_t *w);

/**
 * @brief Fetch the next unseen event in global (ts_utc, seq) order.
 *
 * Scans the hall root recursively, sorts candidate events, and returns the
 * first event whose (ts_utc, seq) is strictly greater than the cursor.
 * On success the cursor is advanced past the returned event.
 *
 * @param w       watch cursor
 * @param out     caller buffer receiving the compact event JSON (no
 *                trailing newline)
 * @param out_sz  buffer size
 * @return 1 when an event was emitted (out filled), 0 when no new events
 *         exist, -1 on error
 */
int gw_hall_watch_next(gw_hall_watch_t *w, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_HALL_STORE_H */
