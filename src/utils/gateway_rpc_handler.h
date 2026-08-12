/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file gateway_rpc_handler.h
 * @brief Unified RPC request handling module.
 *
 * Provides RPC handling logic shared by the HTTP/WS/Stdio gateways,
 * eliminating code duplication per the DRY principle.
 *
 * Design principles:
 *   K-1 minimal core: only protocol translation, zero business logic
 *   K-2 contract interfaces: all functions have full Doxygen comments
 *   E-8 testability: independently unit-testable
 */

/* @owner: team-B */
#ifndef GATEWAY_RPC_HANDLER_H
#define GATEWAY_RPC_HANDLER_H

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stddef.h>

/**
  * @brief RPC result structure
 */
typedef struct {
    char *response_json;
    int error_code;
    const char *error_message;
} rpc_result_t;

/**
  * @brief Unified interface for handling JSON-RPC requests
 *
  * Wraps the full JSON-RPC request handling flow:
  * 1. 1. Validate the request format
  * 2. 2. Extract method and params
  * 3. 3. Call the custom handler or the default syscall routing
  * 4. 4. Generate the response
 *
  * @param[in] request JSON-RPC request object (not freed)
  * @param[in] handler Custom handler callback (may be NULL)
  * @param[in] handler_data Callback user data
  * @return RPC result; caller must free with rpc_result_free()
 *
  * @ownership Return value is owned by the caller
  * @threadsafe safe if the handler is thread-safe
 * @since 1.0.1
 *
 * @code
 * CJSON_PARSE_GUARD(request, json_string, { return NULL; });
 *
 * rpc_result_t result = gateway_rpc_handle_request(request, my_handler, my_data);
 *
 * if (result.error_code != 0) {
 *
 * }
 *
 * printf("Response: %s\n", result.response_json);
 * rpc_result_free(&result);
 *
 * @endcode
 */
rpc_result_t gateway_rpc_handle_request(const cJSON *request,
                                        int (*handler)(const char *, char **, void *),
                                        void *handler_data);

/**
  * @brief Create an RPC error result
  * @param code Error code
  * @param message Error message
  * @return RPC result
 */
rpc_result_t gateway_rpc_create_error(int code, const char *message);

/**
  * @brief Free an RPC result
  * @param result RPC result pointer
 */
void gateway_rpc_free(rpc_result_t *result);

#endif /* GATEWAY_RPC_HANDLER_H */
