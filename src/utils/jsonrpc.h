/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file jsonrpc.h
 * @brief JSON-RPC 2.0 protocol utility functions.
 *
 * Provides standard JSON-RPC 2.0 request validation and response generation,
 * shared by the HTTP and WebSocket gateways.
 */

/* @owner: team-B */
#ifndef GATEWAY_JSONRPC_H
#define GATEWAY_JSONRPC_H

#include <cjson/cJSON.h>

/* Error codes & standard messages: SSoT in commons jsonrpc_helpers.h */
#include "jsonrpc_helpers.h"

/**
  * @brief Validate a JSON-RPC 2.0 request format
 *
  * Checks that required fields (jsonrpc, method, id) are present
  * and validates their types and values against the spec.
 *
  * @param[in] json JSON request object
  * @return 0 valid
  * @return -1 invalid (missing required fields)
  * @return -2 invalid (wrong field types)
  * @return -3 invalid (jsonrpc version is not "2.0")
 */
int gw_jsonrpc_validate_request(const cJSON *json);

/**
  * @brief Create a JSON-RPC 2.0 success response
 *
  * Response format:
 * {
 *   "jsonrpc": "2.0",
 *   "result": <result>,
 *   "id": <id>
 * }
 *
  * @param[in] id Request ID (may be NULL)
  * @param[in] result Result object (may be NULL; null is created)
  * @return JSON string; caller must free()
  * @return NULL on allocation failure
 *
  * @note Ownership of the result object transfers to the response
  * @note A NULL result yields a null in the response
 */
char *jsonrpc_create_success_response(const cJSON *id, cJSON *result);

/**
  * @brief Create a JSON-RPC 2.0 error response
 *
  * Response format:
 * {
 *   "jsonrpc": "2.0",
 *   "error": {
 *     "code": <code>,
 *     "message": "<message>",
 *     "data": <data>
 *   },
 *   "id": <id>
 * }
 *
  * @param[in] id Request ID (may be NULL)
  * @param[in] code Error code
  * @param[in] message Error message (may be NULL; default used)
  * @param[in] data Error data (may be NULL)
  * @return JSON string; caller must free()
  * @return NULL on allocation failure
 *
  * @note Ownership of the data object transfers to the response
 */
char *jsonrpc_create_error_response(const cJSON *id, int code, const char *message, cJSON *data);

#endif /* GATEWAY_JSONRPC_H */
