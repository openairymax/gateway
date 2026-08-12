/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
#include <stdbool.h>
#include <stdint.h>
/**
 * @file gateway_protocol_handler.h
  * @brief Multi-protocol gateway request handler
 *
  * Extends gateway_rpc_handler.h with adaptive handling for MCP/A2A/OpenAI API, etc.
  * Provides protocol detection, conversion and a unified handling interface.
 *
  * Design principles:
  * 1. Backward compatibility - keep full compatibility with existing JSON-RPC clients
  * 2. Protocol adaptivity - auto-detect the request protocol type
  * 3. Unified interface - same API signature as gateway_rpc_handler
  * 4. Extensibility - easy to add new protocol support
 */

#ifndef GATEWAY_PROTOCOL_HANDLER_H
#define GATEWAY_PROTOCOL_HANDLER_H

#include "gateway_rpc_handler.h"
#include "protocol_router.h"
#include "unified_protocol.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================ */

/* ============================================================================ */
/**
  * @brief Protocol handler configuration
 */
typedef struct {
    bool enable_mcp_protocol;
    bool enable_a2a_protocol;
    bool enable_openai_protocol;
    const char *default_protocol;
    uint32_t max_request_size;
    bool enable_protocol_detection;
} gateway_protocol_config_t;

/**
  * @brief Protocol handler handle
 */
typedef struct gateway_protocol_handler_s *gateway_protocol_handler_t;

/* ============================================================================ */

/* ============================================================================ */
/**
  * @brief Create a protocol handler instance
  * @param config Handler configuration (may be NULL for defaults)
  * @return Protocol handler handle, or NULL on failure
 */
gateway_protocol_handler_t gateway_protocol_handler_create(const gateway_protocol_config_t *config);

/**
  * @brief Destroy a protocol handler instance
  * @param handler Protocol handler handle
 */
void gateway_protocol_handler_destroy(gateway_protocol_handler_t handler);

/**
  * @brief Handle a gateway request (multi-protocol)
  * @param handler Protocol handler handle
  * @param request_data Raw request data
  * @param request_size Request data size
  * @param protocol_type Suggested protocol type (may be UNIFIED_PROTOCOL_AUTO for auto-detection)
  * @param custom_handler Custom handler callback (may be NULL for the default syscall routing)
  * @param handler_data Callback user data
  * @return RPC result; caller must free with rpc_result_free()
 *
  * @note Replaces gateway_rpc_handle_request; supports multi-protocol input
  * @note Protocol type is auto-detected when set to UNIFIED_PROTOCOL_AUTO
 */
rpc_result_t gateway_protocol_handle_request(gateway_protocol_handler_t handler,
                                             const char *request_data, size_t request_size,
                                             airy_protocol_type_t protocol_type,
                                             int (*custom_handler)(const char *, char **, void *),
                                             void *handler_data);

/**
  * @brief Get the default protocol handler configuration
  * @param config Configuration output
 */
void gateway_protocol_handler_get_default_config(gateway_protocol_config_t *config);

/**
  * @brief Load protocol handler configuration from JSON
  * @param config Configuration output
  * @param json_config JSON configuration string
  * @return 0 on success, negative error codes on failure
 */
int gateway_protocol_handler_load_config_from_json(gateway_protocol_config_t *config,
                                                   const char *json_config);

/**
  * @brief Get protocol handler statistics
  * @param handler Protocol handler handle
  * @param stats_json Statistics JSON string (output; caller frees)
  * @return 0 on success, negative error codes on failure
 */
int gateway_protocol_handler_get_stats(gateway_protocol_handler_t handler, char **stats_json);

/* ============================================================================ */

/* ============================================================================ */
/**
  * @brief Detect the protocol type of request data
 * @param request_data Request data
  * @param request_size Request data size
  * @return Detected protocol type, or UNIFIED_PROTOCOL_UNKNOWN if undetectable
 */
airy_protocol_type_t gateway_protocol_detect(const char *request_data, size_t request_size);

/**
  * @brief Detect whether the request is JSON-RPC
 * @param request_data Request data
  * @param request_size Request data size
  * @return 1 if JSON-RPC, 0 otherwise
 */
int gateway_protocol_is_jsonrpc(const char *request_data, size_t request_size);

/**
  * @brief Detect whether the request is MCP
 * @param request_data Request data
  * @param request_size Request data size
  * @return 1 if MCP, 0 otherwise
 */
int gateway_protocol_is_mcp(const char *request_data, size_t request_size);

/**
  * @brief Detect whether the request is A2A
 * @param request_data Request data
  * @param request_size Request data size
  * @return 1 if A2A, 0 otherwise
 */
int gateway_protocol_is_a2a(const char *request_data, size_t request_size);

/**
  * @brief Detect whether the request is an OpenAI API request
 * @param request_data Request data
  * @param request_size Request data size
  * @return 1 if OpenAI API, 0 otherwise
 */
int gateway_protocol_is_openai(const char *request_data, size_t request_size);

/* ============================================================================ */

/* ============================================================================ */
/**
  * @brief Convert any protocol request to JSON-RPC
  * @param handler Protocol handler handle
  * @param request_data Raw request data
  * @param request_size Request data size
  * @param protocol_type Source protocol type
  * @param jsonrpc_out Converted JSON-RPC string (output; caller frees)
  * @return 0 on success, negative error codes on failure
 */
int gateway_protocol_convert_to_jsonrpc(gateway_protocol_handler_t handler,
                                        const char *request_data, size_t request_size,
                                        airy_protocol_type_t protocol_type, char **jsonrpc_out);

/**
  * @brief Convert a JSON-RPC response to the target protocol format
  * @param handler Protocol handler handle
  * @param jsonrpc_response JSON-RPC response string
  * @param target_protocol Target protocol type
  * @param target_response Converted response string (output; caller frees)
  * @return 0 on success, negative error codes on failure
 */
int gateway_protocol_convert_from_jsonrpc(gateway_protocol_handler_t handler,
                                          const char *jsonrpc_response,
                                          airy_protocol_type_t target_protocol,
                                          char **target_response);

/* ============================================================================ */

/* ============================================================================ */
/**
  * @brief Backward-compatible interface: handle a JSON-RPC request (same as gateway_rpc_handle_request)
  * @param request JSON-RPC request object
  * @param handler Custom handler callback
  * @param handler_data Callback user data
  * @return RPC result
 *
  * @note Provides full compatibility with existing code
 */
rpc_result_t gateway_protocol_handle_jsonrpc(const cJSON *request,
                                             int (*handler)(const char *, char **, void *),
                                             void *handler_data);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_PROTOCOL_HANDLER_H */