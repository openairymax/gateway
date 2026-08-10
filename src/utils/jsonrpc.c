/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file jsonrpc.c
 * @brief JSON-RPC 2.0 协议工具函数实现
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

// @owner: team-B
#include "jsonrpc.h"

#include "error.h"
#include "error.h"
#include "airy_memory.h"

#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
/* P0.18.2: 引入 cjson_helpers.h 提供 CJSON_PARSE_GUARD/CJSON_AUTO_FREE 宏 */
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

    if (id) {
        cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id, 1));
    } else {
        cJSON_AddNullToObject(response, "id");
    }

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

    if (id) {
        cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id, 1));
    } else {
        cJSON_AddNullToObject(response, "id");
    }

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

/* P0.18.1: jsonrpc_get_error_message 已统一至 daemons/common/src/jsonrpc_helpers.c，
 * 消除 gateway_lib_obj 与 svc_common 链接时的 multiple definition 错误。
 * 声明见 jsonrpc.h:173 及 jsonrpc_helpers.h:56（AIRY_API 权威声明）。 */

int jsonrpc_validate_batch_request(const cJSON *batch_json, size_t *out_count)
{
#ifdef AIRY_HAS_CJSON
    AIRY_CHECK(batch_json != NULL, AIRY_ERR_NULL_POINTER, "batch_json is NULL");
    AIRY_CHECK(out_count != NULL, AIRY_ERR_NULL_POINTER, "out_count is NULL");
    *out_count = 0;

    AIRY_CHECK(cJSON_IsArray(batch_json), AIRY_ERR_INVALID_PARAM,
                  "batch_json is not an array");

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
            char *err_resp = jsonrpc_create_invalid_request_response();
            if (err_resp) {
                /* P0.18.2: 模式 C — 所有权转移至 responses（AddItemToArray） */
                do {
                    CJSON_PARSE_GUARD(parsed, err_resp, { break; });
                    cJSON_AddItemToArray(responses, parsed);
                    parsed = NULL; /* 所有权转移到 responses，防止 CJSON_AUTO_FREE 重复释放 */
                } while (0);
                AIRY_FREE(err_resp);
            }
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
            if (err_resp) {
                /* P0.18.2: 模式 C — 所有权转移至 responses（AddItemToArray） */
                do {
                    CJSON_PARSE_GUARD(parsed, err_resp, { break; });
                    cJSON_AddItemToArray(responses, parsed);
                    parsed = NULL; /* 所有权转移到 responses，防止 CJSON_AUTO_FREE 重复释放 */
                } while (0);
                AIRY_FREE(err_resp);
            }
            continue;
        }

        char *resp_str = handler(item, user_data);
        if (resp_str) {
            /* P0.18.2: 模式 C — 所有权转移至 responses（AddItemToArray） */
            int _resp_parsed_ok = 0;
            do {
                CJSON_PARSE_GUARD(resp_parsed, resp_str, { break; });
                cJSON_AddItemToArray(responses, resp_parsed);
                resp_parsed = NULL; /* 所有权转移到 responses，防止 CJSON_AUTO_FREE 重复释放 */
                _resp_parsed_ok = 1;
            } while (0);
            if (!_resp_parsed_ok) {
                const cJSON *id = jsonrpc_get_id(item);
                char *err_resp_str =
                    jsonrpc_create_internal_error_response(id, "Handler returned invalid JSON");
                if (err_resp_str) {
                    do {
                        CJSON_PARSE_GUARD(err_parsed, err_resp_str, { break; });
                        cJSON_AddItemToArray(responses, err_parsed);
                        err_parsed = NULL; /* 所有权转移到 responses，防止 CJSON_AUTO_FREE 重复释放 */
                    } while (0);
                    AIRY_FREE(err_resp_str);
                }
            }
            AIRY_FREE(resp_str);
        } else {
            const cJSON *id = jsonrpc_get_id(item);
            char *err_resp = jsonrpc_create_internal_error_response(id, "Handler returned NULL");
            if (err_resp) {
                /* P0.18.2: 模式 C — 所有权转移至 responses（AddItemToArray） */
                do {
                    CJSON_PARSE_GUARD(parsed, err_resp, { break; });
                    cJSON_AddItemToArray(responses, parsed);
                    parsed = NULL; /* 所有权转移到 responses，防止 CJSON_AUTO_FREE 重复释放 */
                } while (0);
                AIRY_FREE(err_resp);
            }
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

char *jsonrpc_create_notification(const char *method, cJSON *params)
{
#ifdef AIRY_HAS_CJSON
    if (!method || strlen(method) == 0)
        return NULL;

    cJSON *notif = cJSON_CreateObject();
    if (!notif) {
        if (params)
            cJSON_Delete(params);
        return NULL;
    }

    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", method);

    if (params) {
        cJSON_AddItemToObject(notif, "params", params);
    }

    char *json_str = cJSON_PrintUnformatted(notif);
    cJSON_Delete(notif);

    return json_str;
#else
    (void)method;
    (void)params;
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

char *jsonrpc_create_notification_params(const char *method, const char *params_json)
{
#ifdef AIRY_HAS_CJSON
    if (!method || strlen(method) == 0)
        return NULL;

    if (params_json && strlen(params_json) > 0) {
        /* P0.18.2: 模式 A 变体 — 解析成功后所有权转移给 jsonrpc_create_notification */
        CJSON_PARSE_GUARD(params, params_json, { return NULL; });
        char *result = jsonrpc_create_notification(method, params);
        params = NULL; /* 所有权转移给 notification，防止 CJSON_AUTO_FREE 重复释放 */
        return result;
    }
    return jsonrpc_create_notification(method, NULL);
#else
    (void)method;
    (void)params_json;
    return NULL;
#endif
}
