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

#include <cjson_helpers.h>
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

const char *jsonrpc_get_method(const cJSON *json)
{
#ifdef AIRY_HAS_CJSON
    if (!json) {
        return NULL;
    }
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(json, "method");
    if (!cJSON_IsString(method)) {
        return NULL;
    }
    return method->valuestring;
#else
    (void)json;
    return NULL;
#endif
}

const cJSON *jsonrpc_get_params(const cJSON *json)
{
#ifdef AIRY_HAS_CJSON
    if (!json) {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(json, "params");
#else
    (void)json;
    return NULL;
#endif
}

const cJSON *jsonrpc_get_id(const cJSON *json)
{
#ifdef AIRY_HAS_CJSON
    if (!json) {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(json, "id");
#else
    (void)json;
    return NULL;
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

char *jsonrpc_create_parse_error_response(void)
{
    return jsonrpc_create_error_response(NULL, JSONRPC_PARSE_ERROR, NULL, NULL);
}

char *jsonrpc_create_invalid_request_response(void)
{
    return jsonrpc_create_error_response(NULL, JSONRPC_INVALID_REQUEST, NULL, NULL);
}

char *jsonrpc_create_method_not_found_response(const cJSON *id)
{
    return jsonrpc_create_error_response(id, JSONRPC_METHOD_NOT_FOUND, NULL, NULL);
}

char *jsonrpc_create_invalid_params_response(const cJSON *id, const char *detail)
{
#ifdef AIRY_HAS_CJSON
    cJSON *data = NULL;
    if (detail) {
        data = cJSON_CreateString(detail);
    }
    return jsonrpc_create_error_response(id, JSONRPC_INVALID_PARAMS, NULL, data);
#else
    (void)id;
    (void)detail;
    return NULL;
#endif
}

char *jsonrpc_create_internal_error_response(const cJSON *id, const char *detail)
{
#ifdef AIRY_HAS_CJSON
    cJSON *data = NULL;
    if (detail) {
        data = cJSON_CreateString(detail);
    }
    return jsonrpc_create_error_response(id, JSONRPC_INTERNAL_ERROR, NULL, data);
#else
    (void)id;
    (void)detail;
    return NULL;
#endif
}

char *jsonrpc_create_rate_limited_response(const cJSON *id)
{
    return jsonrpc_create_error_response(id, JSONRPC_RATE_LIMITED, NULL, NULL);
}

char *jsonrpc_create_auth_failed_response(const cJSON *id)
{
    return jsonrpc_create_error_response(id, JSONRPC_AUTH_FAILED, NULL, NULL);
}

/* P0.18.1: jsonrpc_get_error_message moved to daemons/common/src/jsonrpc_helpers.c,
  * fixing the multiple-definition error when linking gateway_lib_obj and svc_common.
  * Declared in jsonrpc.h:173 and jsonrpc_helpers.h:56 (the AIRY_API authority). */

int jsonrpc_validate_batch_request(const cJSON *batch_json, size_t *out_count)
{
#ifdef AIRY_HAS_CJSON
    AIRY_CHECK(batch_json != NULL, AIRY_ERR_NULL_POINTER, "batch_json is NULL");
    AIRY_CHECK(out_count != NULL, AIRY_ERR_NULL_POINTER, "out_count is NULL");
    *out_count = 0;

    AIRY_CHECK(cJSON_IsArray(batch_json), AIRY_ERR_INVALID_PARAM, "batch_json is not an array");

    size_t count = cJSON_GetArraySize(batch_json);
    AIRY_CHECK(count > 0, AIRY_ERR_INVALID_PARAM, "batch is empty");
    if (count > JSONRPC_MAX_BATCH_SIZE)
        AIRY_ERROR(-3, "batch exceeds max size");

    int has_invalid = 0;
    for (size_t i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(batch_json, (int)i);
        if (!cJSON_IsObject(item)) {
            has_invalid = 1;
            continue;
        }
        (*out_count)++;
    }

    return has_invalid ? -4 : 0;
#else
    (void)batch_json;
    (void)out_count;
    AIRY_ERROR(AIRY_ERR_NOT_SUPPORTED, "cJSON not available");
#endif
}

#ifdef AIRY_HAS_CJSON
/* parse a response string, append it to the batch; consumes json_str */
static bool batch_append(cJSON *responses, char *json_str)
{
    bool ok = false;

    if (json_str) {
        do {
            CJSON_PARSE_GUARD(parsed, json_str, { break; });
            cJSON_AddItemToArray(responses, parsed);
            parsed = NULL;
            ok = true;
        } while (0);
        AIRY_FREE(json_str);
    }
    return ok;
}
#endif

char *jsonrpc_process_batch(const cJSON *batch_json,
                            char *(*handler)(const cJSON *request, void *user_data),
                            void *user_data)
{
#ifdef AIRY_HAS_CJSON
    if (!batch_json || !handler || !cJSON_IsArray(batch_json)) {
        return NULL;
    }

    size_t count = (size_t)cJSON_GetArraySize(batch_json);
    if (count > JSONRPC_MAX_BATCH_SIZE)
        count = JSONRPC_MAX_BATCH_SIZE;

    cJSON *responses = cJSON_CreateArray();
    if (!responses)
        return NULL;

    for (size_t i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(batch_json, (int)i);

        if (!cJSON_IsObject(item)) {
            batch_append(responses, jsonrpc_create_invalid_request_response());
            continue;
        }

        if (gw_jsonrpc_is_notification(item)) {
            continue;
        }

        int valid = gw_jsonrpc_validate_request(item);
        if (valid != 0) {
            (void)jsonrpc_get_id(item);
            char *err_resp = NULL;
            switch (valid) {
            case -3:
                err_resp = jsonrpc_create_parse_error_response();
                break;
            case -2:
                err_resp = jsonrpc_create_invalid_request_response();
                break;
            default:
                err_resp = jsonrpc_create_invalid_request_response();
                break;
            }
            batch_append(responses, err_resp);
            continue;
        }

        char *resp_str = handler(item, user_data);
        if (resp_str) {
            if (!batch_append(responses, resp_str)) {
                const cJSON *id = jsonrpc_get_id(item);
                char *err_resp_str =
                    jsonrpc_create_internal_error_response(id, "Handler returned invalid JSON");
                batch_append(responses, err_resp_str);
            }
        } else {
            const cJSON *id = jsonrpc_get_id(item);
            char *err_resp = jsonrpc_create_internal_error_response(id, "Handler returned NULL");
            batch_append(responses, err_resp);
        }
    }

    char *result = cJSON_PrintUnformatted(responses);
    cJSON_Delete(responses);
    return result;
#else
    (void)batch_json;
    (void)handler;
    (void)user_data;
    return NULL;
#endif
}

bool gw_jsonrpc_is_notification(const cJSON *json)
{
#ifdef AIRY_HAS_CJSON
    if (!json || !cJSON_IsObject(json))
        return false;

    return !cJSON_HasObjectItem(json, "id");
#else
    (void)json;
    return false;
#endif
}
