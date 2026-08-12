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
#include <stdbool.h>
#include <stddef.h>


#define JSONRPC_PARSE_ERROR (-32700)
#define JSONRPC_INVALID_REQUEST (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INVALID_PARAMS (-32602)
#define JSONRPC_INTERNAL_ERROR (-32603)

#define JSONRPC_SERVER_ERROR_BASE (-32000)
#define JSONRPC_RATE_LIMITED (-32001)
#define JSONRPC_AUTH_FAILED (-32002)
#define JSONRPC_SESSION_EXPIRED (-32003)
#define JSONRPC_SERVICE_UNAVAILABLE (-32004)

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
  * @brief Extract the method name from a JSON-RPC request
 *
  * @param[in] json JSON request object
  * @return Method name string pointer (no ownership transfer)
  * @return NULL if the request is invalid or has no method field
 */
const char *jsonrpc_get_method(const cJSON *json);

/**
  * @brief Extract parameters from a JSON-RPC request
 *
  * @param[in] json JSON request object
  * @return Parameter object pointer (no ownership transfer)
  * @return NULL if no parameters
 */
const cJSON *jsonrpc_get_params(const cJSON *json);

/**
  * @brief Extract the ID from a JSON-RPC request
 *
  * @param[in] json JSON request object
  * @return ID object pointer (no ownership transfer)
  * @return NULL if no ID field
 */
const cJSON *jsonrpc_get_id(const cJSON *json);


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


/**
  * @brief Create a parse error response
 */
char *jsonrpc_create_parse_error_response(void);

/**
  * @brief Create an invalid request response
 */
char *jsonrpc_create_invalid_request_response(void);

/**
  * @brief Create a method-not-found response
 */
char *jsonrpc_create_method_not_found_response(const cJSON *id);

/**
  * @brief Create an invalid params response
 */
char *jsonrpc_create_invalid_params_response(const cJSON *id, const char *detail);

/**
  * @brief Create an internal error response
 */
char *jsonrpc_create_internal_error_response(const cJSON *id, const char *detail);

/**
  * @brief Create a rate-limited response
 */
char *jsonrpc_create_rate_limited_response(const cJSON *id);

/**
  * @brief Create an authentication failure response
 */
char *jsonrpc_create_auth_failed_response(const cJSON *id);


/**
  * @brief Get the standard error message
 *
  * @param[in] code Error code
  * @return Error message string
 */
const char *jsonrpc_get_error_message(int code);

/* ==================== Batch Requests (PROTO-004) ==================== */
#define JSONRPC_MAX_BATCH_SIZE 64

/**
  * @brief Validate and parse a batch request (JSON array)
 *
  * Batch format: [{req1}, {req2}, ...]
  * Each sub-request must conform to JSON-RPC 2.0.
 *
  * @param[in] batch_json JSON array object
  * @param[out] out_count Parsed request count
  * @return 0 success
  * @return -1 input is NULL or not an array
  * @return -2 array element is not an object
  * @return -3 exceeds the maximum batch size
  * @return -4 contains invalid sub-requests (partial success)
 */
int jsonrpc_validate_batch_request(const cJSON *batch_json, size_t *out_count);

/**
  * @brief Handle a batch request and return a response array
 *
  * Calls the handler for each sub-request and collects all responses.
  * Returns the successful responses even if some fail.
 *
  * @param[in] batch_json Batch request array
  * @param[in] handler Single-request handler callback
  * @param[in] user_data User data passed to the handler
  * @return JSON response array string (free required), NULL on failure
 */
char *jsonrpc_process_batch(const cJSON *batch_json,
                            char *(*handler)(const cJSON *request, void *user_data),
                            void *user_data);

/* ==================== Notifications (PROTO-004) ==================== */
/**
  * @brief Create a notification (a request without an id)
 *
  * Notification format:
 * {
 *   "jsonrpc": "2.0",
 *   "method": "<method>",
 *   "params": <params>
 * }
 *
  * Unlike a normal request, no "id" field; the server must not reply.
 *
 * @param[in] method Method name
 * @param[in] params Parameter object (may be NULL)
  * @return JSON notification string (free required), NULL on failure
 */
char *jsonrpc_create_notification(const char *method, cJSON *params);

/**
  * @brief Check whether a request is a notification (no id)
 *
 * @param[in] json JSON object
  * @return true if a notification
  * @return false otherwise (has an id)
 */
bool gw_jsonrpc_is_notification(const cJSON *json);

/**
  * @brief Create a parameterized notification (convenience)
 *
 * @param[in] method Method name
 * @param[in] params_json Parameter JSON string
  * @return JSON notification string (free required)
 */
char *jsonrpc_create_notification_params(const char *method, const char *params_json);

#endif /* GATEWAY_JSONRPC_H */
