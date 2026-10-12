// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file jsonrpc.c
 * @brief JSON-RPC 2.0 protocol utility functions implementation.
 */

// @owner: team-B
#include "jsonrpc.h"

#include "error.h"
#include "airy_memory.h"

#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

int gw_jsonrpc_validate_request(const cJSON *json)
{
#ifdef AIRY_HAS_CJSON
    AIRY_CHECK(json != NULL, AIRY_ERR_NULL_POINTER, "json is NULL");

    if (!cJSON_HasObjectItem(json, "jsonrpc") || !cJSON_HasObjectItem(json, "method") ||
        !cJSON_HasObjectItem(json, "id")) {
        AIRY_ERROR(AIRY_ERR_NOT_FOUND, "missing required JSON-RPC fields");
    }

    const cJSON *jsonrpc = cJSON_GetObjectItemCaseSensitive(json, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(json, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");

    if (!cJSON_IsString(jsonrpc)) {
        AIRY_ERROR(-2, "jsonrpc field is not a string");
    }
    if (strcmp(jsonrpc->valuestring, "2.0") != 0) {
        AIRY_ERROR(-3, "jsonrpc version is not 2.0");
    }

    if (!cJSON_IsString(method)) {
        AIRY_ERROR(-2, "method field is not a string");
    }
    if (strlen(method->valuestring) == 0) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "method is empty");
    }

    if (!cJSON_IsNumber(id) && !cJSON_IsString(id) && !cJSON_IsNull(id)) {
        AIRY_ERROR(-2, "id field has invalid type");
    }

    return 0;
#else
    (void)json;
    AIRY_ERROR(AIRY_ERR_NOT_SUPPORTED, "cJSON not available");
#endif
}

#ifdef AIRY_HAS_CJSON
/* echo the request id into a response envelope */
static void jsonrpc_add_id(cJSON *response, const cJSON *id)
{
    if (id)
        cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddNullToObject(response, "id");
}
#endif

char *jsonrpc_create_success_response(const cJSON *id, cJSON *result)
{
#ifdef AIRY_HAS_CJSON
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        if (result)
            cJSON_Delete(result);
        return NULL;
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");

    if (result) {
        cJSON_AddItemToObject(response, "result", result);
    } else {
        cJSON_AddNullToObject(response, "result");
    }

    jsonrpc_add_id(response, id);

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    return json_str;
#else
    (void)id;
    (void)result;
    return NULL;
#endif
}

char *jsonrpc_create_error_response(const cJSON *id, int code, const char *message, cJSON *data)
{
#ifdef AIRY_HAS_CJSON
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        if (data)
            cJSON_Delete(data);
        return NULL;
    }

    cJSON *error = cJSON_CreateObject();
    if (!error) {
        cJSON_Delete(response);
        if (data)
            cJSON_Delete(data);
        return NULL;
    }

    cJSON_AddNumberToObject(error, "code", code);

    const char *msg = message;
    if (!msg) {
        msg = jsonrpc_get_error_message(code);
    }
    cJSON_AddStringToObject(error, "message", msg ? msg : "Internal error");

    if (data) {
        cJSON_AddItemToObject(error, "data", data);
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "error", error);

    jsonrpc_add_id(response, id);

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    return json_str;
#else
    (void)id;
    (void)code;
    (void)message;
    (void)data;
    return NULL;
#endif
}

/* P0.18.1: jsonrpc_get_error_message moved to daemons/common/src/jsonrpc_helpers.c,
  * fixing the multiple-definition error when linking gateway_lib_obj and svc_common.
  * Declared in jsonrpc.h:173 and jsonrpc_helpers.h:56 (the AIRY_API authority). */
