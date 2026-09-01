/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
/**
 * @file gateway_business_handler.h
 * @brief Gateway business-request handler (agent.run -> llm_d forwarding).
 *
 * Provides the HTTP gateway's default business chain:
 *   standard JSON-RPC agent.run -> llm_d(complete) -> JSON-RPC result
 * Fixes the previous gap where the HTTP gateway handler was never wired up
 * (always returning "Custom handler failed").
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_BUSINESS_HANDLER_H
#define AIRY_RT_DAEMON_GATEWAY_BUSINESS_HANDLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct gw_proto_router gw_proto_router_t;

/** @brief Business-handler context. */
typedef struct gateway_business_ctx_s gateway_business_ctx_t;

/** @brief Unified protocol-entry context: protocol router + JSON-RPC biz. */
typedef struct {
    gateway_business_ctx_t *biz_ctx;
    gw_proto_router_t *router;
} gateway_entry_ctx_t;

/**
 * @brief Create a business-handler context.
 *
 * Resolves the llm_d endpoint from environment variables:
 *   - AIRY_LLM_SOCK: POSIX Unix socket path (default $AIRY_RUNTIME_DIR/llm.sock)
 *   - AIRY_LLM_TCP_ADDR / AIRY_LLM_TCP_PORT: Windows TCP endpoint
 *     (default 127.0.0.1:8080)
 *
 * @return Context pointer, NULL on failure
 */
gateway_business_ctx_t *gateway_business_ctx_create(void);

/**
 * @brief Destroy a business-handler context.
 * @param ctx Context pointer
 */
void gateway_business_ctx_destroy(gateway_business_ctx_t *ctx);

/**
 * @brief L2 standard method <ns>.shutdown callback type
 *        (02-l2-service-protocol.md §6.1).
 *
 * Called by gateway_business_handle on the "shutdown" method; the host
 * (gateway_d main) triggers the real graceful exit (e.g. atomically
 * clearing g_running so the main loop exits).
 */
typedef void (*gateway_shutdown_fn_t)(void *user_data);

/**
 * @brief Set the shutdown callback (L2 <ns>.shutdown support).
 * @param ctx Business-handler context
 * @param cb  Shutdown callback (NULL = unsupported)
 * @param user_data Callback user data (e.g. &g_running)
 * @return 0 on success, non-zero on failure
 */
int gateway_business_ctx_set_shutdown_cb(gateway_business_ctx_t *ctx, gateway_shutdown_fn_t cb,
                                         void *user_data);

/**
 * @brief Gateway business-request handler (gateway_service_handler_t sig).
 *
 * Takes a standard JSON-RPC request string; supports:
 *   - "ping"      -> {"result":{"status":"ok"}}
 *   - "agent.run" -> forwards to llm_d.complete, returns the chat result
 *   - other       -> -32601 Method not found
 *
 * @param request JSON-RPC request string
 * @param user_data gateway_business_ctx_t*
 * @return JSON response string (AIRY_MALLOC-allocated), NULL on failure
 */
char *gateway_business_handle(void *request, void *user_data);


/**
 * @brief Unified protocol entry (replaces gateway_business_handle as the
 *        sole HTTP handler).
 *
 * Detects the protocol from the body and routes:
 *   - MCP / OpenAI / A2A -> matching adapter (internal service calls below)
 *   - other JSON-RPC (agent.run/ping) -> gateway_business_handle
 *
 * @param request JSON-RPC request string
 * @param user_data gateway_entry_ctx_t*
 * @return JSON response string (AIRY_MALLOC-allocated), NULL on failure
 */
char *gateway_protocol_entry(void *request, void *user_data);

/**
 * @brief MCP tool-execution backend: tools/call -> tool_d.execute_tool.
 * @param tool_name Tool name (fs_read/fs_write/fs_list/shell_run)
 * @param arguments_json Tool-argument JSON string
 * @param result_json Output result (valid JSON string, AIRY_MALLOC, caller AIRY_FREEs)
 * @param user_data gateway_business_ctx_t*
 * @return 0 on success, non-zero on failure
 */
int gw_biz_tool_exec(const char *tool_name, const char *arguments_json, char **result_json,
                     void *user_data);

/**
 * @brief OpenAI LLM backend: chat/completions -> llm_d.complete.
 * @param model Model name
 * @param messages_json Chat-messages array JSON
 * @param functions_json OpenAI tools/functions array JSON (may be NULL)
 * @param temperature Sampling temperature
 * @param max_tokens Max token count
 * @param response_json OpenAI chat.completion-format response (AIRY_MALLOC, caller AIRY_FREEs)
 * @param user_data gateway_business_ctx_t*
 * @return 0 on success, non-zero on failure
 */
int gw_biz_llm_complete(const char *model, const char *messages_json, const char *functions_json,
                        double temperature, int max_tokens, char **response_json, void *user_data);

/**
 * @brief OpenAI embeddings backend: /v1/embeddings -> llm_d.embeddings.
 * @param model Embedding model name (may be NULL/empty → gateway default)
 * @param input_json OpenAI input field JSON ("text" or ["t1","t2"])
 * @param response_json Upstream OpenAI-format embeddings JSON (AIRY_MALLOC, caller AIRY_FREEs)
 * @param user_data gateway_business_ctx_t*
 * @return 0 on success, non-zero on failure
 */
int gw_biz_llm_embeddings(const char *model, const char *input_json, char **response_json,
                          void *user_data);

/**
 * @brief A2A task backend: task -> sched_d.schedule_task.
 * @param task_id Task ID
 * @param task_type Task type (encoding/analysis, etc.)
 * @param input_json Task-input JSON string
 * @param output_json Scheduling result (valid JSON, AIRY_MALLOC, caller AIRY_FREEs)
 * @param user_data gateway_business_ctx_t*
 * @return 0 on success, non-zero on failure
 */
int gw_biz_sched_schedule(const char *task_id, const char *task_type, const char *input_json,
                          char **output_json, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_BUSINESS_HANDLER_H */
