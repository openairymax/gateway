// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_rpc_handler.c
 * @brief Unified RPC request handling implementation.
 *
 * Implements RPC handling shared by the HTTP/WS/Stdio gateways, keeping
 * cyclomatic complexity below 7 for maintainability.
 */

// @owner: team-B
#include "gateway_rpc_handler.h"

#include "error.h"
#include "error.h"
#include "jsonrpc.h"
#include "airy_memory.h"
#include "syscall_router.h"

#include <stdlib.h>
#include <string.h>

/**
  * @brief Validate the JSON-RPC request format (CC=3)
 */
static int validate_rpc_request(const cJSON *request)
{
    AIRY_CHECK(request != NULL, AIRY_ERR_NULL_POINTER, "request is NULL");
    AIRY_CHECK(cJSON_IsObject(request), AIRY_ERR_INVALID_PARAM, "request is not a JSON object");

    const cJSON *jsonrpc = cJSON_GetObjectItem(request, "jsonrpc");
    AIRY_CHECK(jsonrpc != NULL, AIRY_ERR_NOT_FOUND, "jsonrpc field missing");
    AIRY_CHECK(cJSON_IsString(jsonrpc), AIRY_ERR_INVALID_PARAM, "jsonrpc is not a string");
    AIRY_CHECK(strcmp(jsonrpc->valuestring, "2.0") == 0, AIRY_ERR_NOT_SUPPORTED,
               "jsonrpc version is not 2.0");

    const cJSON *method = cJSON_GetObjectItem(request, "method");
    AIRY_CHECK(method != NULL, AIRY_ERR_NOT_FOUND, "method field missing");
    AIRY_CHECK(cJSON_IsString(method), AIRY_ERR_INVALID_PARAM, "method is not a string");

    const cJSON *id = cJSON_GetObjectItem(request, "id");
    if (id && !cJSON_IsNumber(id) && !cJSON_IsString(id) && !cJSON_IsNull(id))
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "id type is invalid");

    return 0;
}

/**
  * @brief Extract request fields (CC=4)
 */
static int extract_request_fields(const cJSON *request, const char **method_out,
                                  const cJSON **params_out, const cJSON **id_out)
{
    AIRY_CHECK(request != NULL, AIRY_ERR_NULL_POINTER, "request is NULL");
    AIRY_CHECK(method_out != NULL, AIRY_ERR_NULL_POINTER, "method_out is NULL");
    AIRY_CHECK(params_out != NULL, AIRY_ERR_NULL_POINTER, "params_out is NULL");
    AIRY_CHECK(id_out != NULL, AIRY_ERR_NULL_POINTER, "id_out is NULL");

    *method_out = NULL;
    *params_out = NULL;
    *id_out = NULL;

    const cJSON *method = cJSON_GetObjectItem(request, "method");
    AIRY_CHECK(method != NULL, AIRY_ERR_NOT_FOUND, "method field missing");
    AIRY_CHECK(cJSON_IsString(method), AIRY_ERR_INVALID_PARAM, "method is not a string");
    *method_out = method->valuestring;

    *params_out = cJSON_GetObjectItem(request, "params");

    *id_out = cJSON_GetObjectItem(request, "id");

    return 0;
}

rpc_result_t gateway_rpc_handle_request(const cJSON *request,
                                        int (*handler)(const char *, char **, void *),
                                        void *handler_data)
{
    rpc_result_t result = {NULL, 0, NULL};

    if (!request) {
        result = gateway_rpc_create_error(-32600, "Invalid request: NULL");
        return result;
    }

    if (validate_rpc_request(request) != 0) {
        result = gateway_rpc_create_error(-32600, "Invalid Request");
        return result;
    }

    const char *method = NULL;
    const cJSON *params = NULL;
    const cJSON *id = NULL;

    if (extract_request_fields(request, &method, &params, &id) != 0) {
        result = gateway_rpc_create_error(-32600, "Missing required fields");
        return result;
    }

    char *response_str = NULL;

    if (handler) {

        char *request_str = cJSON_PrintUnformatted((cJSON *)request);
        if (!request_str) {
            result = gateway_rpc_create_error(-32000, "Memory allocation failed");
            return result;
        }

        int ret = handler(request_str, &response_str, handler_data);
        AIRY_FREE(request_str);

        if (ret != 0 || !response_str) {
            result = gateway_rpc_create_error(-32000, "Handler error");
            return result;
        }
    } else {

        response_str = gateway_syscall_route(method, (cJSON *)params, (cJSON *)id);

        if (!response_str) {
            result = gateway_rpc_create_error(-32000, "Internal error");
            return result;
        }
    }

    result.response_json = response_str;
    result.error_code = 0;
    result.error_message = NULL;

    return result;
}

rpc_result_t gateway_rpc_create_error(int code, const char *message)
{
    rpc_result_t result = {NULL, 0, NULL};

    result.response_json =
        jsonrpc_create_error_response(NULL, code, message ? message : "Unknown error", NULL);

    if (result.response_json) {
        result.error_code = code;
        result.error_message = message;
    } else {
        result.error_code = -32700;
        result.error_message = "Failed to create error response";
    }

    return result;
}

void gateway_rpc_free(rpc_result_t *result)
{
    if (!result)
        return;

    if (result->response_json) {
        AIRY_FREE(result->response_json);
        result->response_json = NULL;
    }

    result->error_code = 0;
    result->error_message = NULL;
}
