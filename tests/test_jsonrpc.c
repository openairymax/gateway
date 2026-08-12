// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_jsonrpc.c
 * @brief JSON-RPC 2.0 协议工具单元测试
 *
 * 测试 JSON-RPC 协议工具的完整功能：
  * - 请求验证
  * - 响应生成
  * - 错误处理
 * - 边界条件
 *
  * Design principles:
  *   E-8 可测试性：协议合规性验证
  *   K-2 接口契约化：验证 JSON-RPC 2.0 标准
 *
 */

// @owner: team-B
#include "jsonrpc.h"

#include "error.h"

#include <assert.h>
#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_BEGIN(name)                  \
    do {                                  \
        printf("  [TEST] %s ... ", name); \
        g_tests_run++;                    \
    } while (0)

#define TEST_PASS()       \
    do {                  \
        printf("PASS\n"); \
        g_tests_passed++; \
    } while (0)

#define TEST_FAIL(msg)             \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define ASSERT_TRUE(cond)     \
    do {                      \
        if (!(cond)) {        \
            TEST_FAIL(#cond); \
            return;           \
        }                     \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_NULL(ptr) ASSERT_TRUE((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT_TRUE((ptr) != NULL)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

static cJSON *parse_json(const char *json_str)
{
    if (!json_str)
        return NULL;
    cJSON *json = cJSON_Parse(json_str);
    if (!json)
        return NULL;
    return json;
}

/**
  * @brief Test valid JSON-RPC request validation
 */
static void test_validate_valid_request(void)
{
    TEST_BEGIN("validate_valid_request");

    const char *valid_request = "{\"jsonrpc\":\"2.0\",\"method\":\"test\",\"id\":1}";
    cJSON *json = parse_json(valid_request);

    int result = gw_jsonrpc_validate_request(json);
    ASSERT_EQ(result, 0);

    const char *method = jsonrpc_get_method(json);
    ASSERT_NOT_NULL(method);
    ASSERT_STR_EQ(method, "test");

    const cJSON *id = jsonrpc_get_id(json);
    ASSERT_NOT_NULL(id);
    ASSERT_EQ(id->valueint, 1);

    cJSON_Delete(json);

    TEST_PASS();
}

/**
  * @brief Test a valid request with parameters
 */
static void test_validate_request_with_params(void)
{
    TEST_BEGIN("validate_request_with_params");

    const char *valid_request = "{\"jsonrpc\":\"2.0\",\"method\":\"test\","
                                "\"params\":{\"key\":\"value\"},\"id\":\"req1\"}";
    cJSON *json = parse_json(valid_request);

    int result = gw_jsonrpc_validate_request(json);
    ASSERT_EQ(result, 0);

    const cJSON *params = jsonrpc_get_params(json);
    ASSERT_NOT_NULL(params);

    cJSON *key = cJSON_GetObjectItem(params, "key");
    ASSERT_NOT_NULL(key);
    ASSERT_STR_EQ(key->valuestring, "value");

    cJSON_Delete(json);

    TEST_PASS();
}

/**
  * @brief Test requests missing required fields
 */
static void test_validate_missing_fields(void)
{
    TEST_BEGIN("validate_missing_fields");

    const char *missing_jsonrpc = "{\"method\":\"test\",\"id\":1}";
    cJSON *json = parse_json(missing_jsonrpc);
    int result = gw_jsonrpc_validate_request(json);
    ASSERT_TRUE(result < 0);
    cJSON_Delete(json);

    const char *missing_method = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    json = parse_json(missing_method);
    result = gw_jsonrpc_validate_request(json);
    ASSERT_TRUE(result < 0);
    cJSON_Delete(json);

    const char *missing_id = "{\"jsonrpc\":\"2.0\",\"method\":\"test\"}";
    json = parse_json(missing_id);
    result = gw_jsonrpc_validate_request(json);
    ASSERT_TRUE(result < 0);
    cJSON_Delete(json);

    TEST_PASS();
}

/**
  * @brief Test jsonrpc version errors
 */
static void test_validate_wrong_version(void)
{
    TEST_BEGIN("validate_wrong_version");

    const char *wrong_version = "{\"jsonrpc\":\"1.0\",\"method\":\"test\",\"id\":1}";
    cJSON *json = parse_json(wrong_version);

    int result = gw_jsonrpc_validate_request(json);
    ASSERT_TRUE(result < 0);

    cJSON_Delete(json);

    TEST_PASS();
}

/**
  * @brief Test field type errors
 */
static void test_validate_wrong_field_types(void)
{
    TEST_BEGIN("validate_wrong_field_types");

    const char *method_not_string = "{\"jsonrpc\":\"2.0\",\"method\":123,\"id\":1}";
    cJSON *json = parse_json(method_not_string);
    int result = gw_jsonrpc_validate_request(json);
    ASSERT_TRUE(result < 0);
    cJSON_Delete(json);

    const char *id_is_object = "{\"jsonrpc\":\"2.0\",\"method\":\"test\",\"id\":{}}";
    json = parse_json(id_is_object);
    result = gw_jsonrpc_validate_request(json);
    ASSERT_TRUE(result < 0);
    cJSON_Delete(json);

    TEST_PASS();
}

/**
  * @brief Test success response creation
 */
static void test_create_success_response(void)
{
    TEST_BEGIN("create_success_response");

    cJSON *id = cJSON_CreateNumber(1);
    ASSERT_NOT_NULL(id);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "success");
    ASSERT_NOT_NULL(result);

    char *response = jsonrpc_create_success_response(id, result);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *version = cJSON_GetObjectItem(resp_json, "jsonrpc");
    ASSERT_NOT_NULL(version);
    ASSERT_STR_EQ(version->valuestring, "2.0");

    cJSON *result_field = cJSON_GetObjectItem(resp_json, "result");
    ASSERT_NOT_NULL(result_field);

    cJSON *id_field = cJSON_GetObjectItem(resp_json, "id");
    ASSERT_NOT_NULL(id_field);
    ASSERT_EQ(id_field->valueint, 1);

    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    ASSERT_NULL(error);

    cJSON_free(response);

    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test responses with a NULL result
 */
static void test_create_success_response_null_result(void)
{
    TEST_BEGIN("create_success_response_null_result");

    cJSON *id = cJSON_CreateNumber(2);
    ASSERT_NOT_NULL(id);

    char *response = jsonrpc_create_success_response(id, NULL);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *result = cJSON_GetObjectItem(resp_json, "result");
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->type == cJSON_NULL);

    cJSON_free(response);
    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test error response creation
 */
static void test_create_error_response(void)
{
    TEST_BEGIN("create_error_response");

    cJSON *id = cJSON_CreateNumber(3);
    ASSERT_NOT_NULL(id);

    char *response = jsonrpc_create_error_response(id, -32601, "Method not found", NULL);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    ASSERT_NOT_NULL(error);

    cJSON *code = cJSON_GetObjectItem(error, "code");
    ASSERT_NOT_NULL(code);
    ASSERT_EQ(code->valueint, -32601);

    cJSON *message = cJSON_GetObjectItem(error, "message");
    ASSERT_NOT_NULL(message);
    ASSERT_STR_EQ(message->valuestring, "Method not found");

    cJSON *id_field = cJSON_GetObjectItem(resp_json, "id");
    ASSERT_NOT_NULL(id_field);
    ASSERT_EQ(id_field->valueint, 3);

    cJSON *result = cJSON_GetObjectItem(resp_json, "result");
    ASSERT_NULL(result);

    cJSON_free(response);
    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test error responses with detailed data
 */
static void test_create_error_response_with_data(void)
{
    TEST_BEGIN("create_error_response_with_data");

    cJSON *id = cJSON_CreateNumber(4);
    cJSON *data = cJSON_CreateString("Additional error details");

    char *response = jsonrpc_create_error_response(id, -32000, "Server error", data);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    ASSERT_NOT_NULL(error);

    cJSON *data_field = cJSON_GetObjectItem(error, "data");
    ASSERT_NOT_NULL(data_field);
    ASSERT_STR_EQ(data_field->valuestring, "Additional error details");

    cJSON_free(response);

    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test parse error responses
 */
static void test_create_parse_error_response(void)
{
    TEST_BEGIN("create_parse_error_response");

    char *response = jsonrpc_create_parse_error_response();
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    ASSERT_NOT_NULL(error);

    cJSON *code = cJSON_GetObjectItem(error, "code");
    ASSERT_NOT_NULL(code);
    ASSERT_EQ(code->valueint, JSONRPC_PARSE_ERROR);

    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Test invalid request responses
 */
static void test_create_invalid_request_response(void)
{
    TEST_BEGIN("create_invalid_request_response");

    char *response = jsonrpc_create_invalid_request_response();
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });
    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    cJSON *code = cJSON_GetObjectItem(error, "code");

    ASSERT_EQ(code->valueint, JSONRPC_INVALID_REQUEST);

    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Test method-not-found responses
 */
static void test_create_method_not_found_response(void)
{
    TEST_BEGIN("create_method_not_found_response");

    cJSON *id = cJSON_CreateNumber(5);
    char *response = jsonrpc_create_method_not_found_response(id);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });
    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    cJSON *code = cJSON_GetObjectItem(error, "code");

    ASSERT_EQ(code->valueint, JSONRPC_METHOD_NOT_FOUND);

    cJSON_free(response);
    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test invalid params responses
 */
static void test_create_invalid_params_response(void)
{
    TEST_BEGIN("create_invalid_params_response");

    cJSON *id = cJSON_CreateNumber(6);
    char *response = jsonrpc_create_invalid_params_response(id, "Missing required field");
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });
    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    cJSON *code = cJSON_GetObjectItem(error, "code");
    cJSON *message = cJSON_GetObjectItem(error, "message");

    ASSERT_EQ(code->valueint, JSONRPC_INVALID_PARAMS);
    ASSERT_STR_EQ(message->valuestring, "Invalid params");

    cJSON_free(response);
    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test internal error responses
 */
static void test_create_internal_error_response(void)
{
    TEST_BEGIN("create_internal_error_response");

    cJSON *id = cJSON_CreateNumber(7);
    char *response = jsonrpc_create_internal_error_response(id, "Unexpected error");
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });
    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    cJSON *code = cJSON_GetObjectItem(error, "code");

    ASSERT_EQ(code->valueint, JSONRPC_INTERNAL_ERROR);

    cJSON_free(response);
    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test rate-limit responses
 */
static void test_create_rate_limited_response(void)
{
    TEST_BEGIN("create_rate_limited_response");

    cJSON *id = cJSON_CreateNumber(8);
    char *response = jsonrpc_create_rate_limited_response(id);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });
    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    cJSON *code = cJSON_GetObjectItem(error, "code");

    ASSERT_EQ(code->valueint, JSONRPC_RATE_LIMITED);

    cJSON_free(response);
    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test auth-failure responses
 */
static void test_create_auth_failed_response(void)
{
    TEST_BEGIN("create_auth_failed_response");

    cJSON *id = cJSON_CreateNumber(9);
    char *response = jsonrpc_create_auth_failed_response(id);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });
    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    cJSON *code = cJSON_GetObjectItem(error, "code");

    ASSERT_EQ(code->valueint, JSONRPC_AUTH_FAILED);

    cJSON_free(response);
    cJSON_Delete(id);

    TEST_PASS();
}

/**
  * @brief Test standard error message lookup
 */
static void test_get_error_message(void)
{
    TEST_BEGIN("get_error_message");

    const char *msg;

    msg = jsonrpc_get_error_message(JSONRPC_PARSE_ERROR);
    ASSERT_NOT_NULL(msg);
    ASSERT_TRUE(strlen(msg) > 0);

    msg = jsonrpc_get_error_message(JSONRPC_INVALID_REQUEST);
    ASSERT_NOT_NULL(msg);
    ASSERT_TRUE(strlen(msg) > 0);

    msg = jsonrpc_get_error_message(JSONRPC_METHOD_NOT_FOUND);
    ASSERT_NOT_NULL(msg);
    ASSERT_TRUE(strlen(msg) > 0);

    msg = jsonrpc_get_error_message(JSONRPC_INVALID_PARAMS);
    ASSERT_NOT_NULL(msg);
    ASSERT_TRUE(strlen(msg) > 0);

    msg = jsonrpc_get_error_message(JSONRPC_INTERNAL_ERROR);
    ASSERT_NOT_NULL(msg);
    ASSERT_TRUE(strlen(msg) > 0);

    TEST_PASS();
}

/**
  * @brief Test NULL input handling
 */
static void test_null_input_handling(void)
{
    TEST_BEGIN("null_input_handling");

    int result = gw_jsonrpc_validate_request(NULL);
    ASSERT_TRUE(result < 0);

    const char *method = jsonrpc_get_method(NULL);
    ASSERT_NULL(method);

    const cJSON *params = jsonrpc_get_params(NULL);
    ASSERT_NULL(params);

    const cJSON *id = jsonrpc_get_id(NULL);
    ASSERT_NULL(id);

    TEST_PASS();
}

/**
  * @brief Test allocation-failure scenarios (simulated via NULL args)
 */
static void test_memory_allocation_failure(void)
{
    TEST_BEGIN("memory_allocation_failure_simulation");

    char *response = jsonrpc_create_success_response(NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = jsonrpc_create_error_response(NULL, -32600, "Test error", NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    TEST_PASS();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n========================================\n");
    printf("  JSON-RPC 2.0 Protocol Unit Tests v1.0\n");
    printf("  (Testing protocol compliance)\n");
    printf("========================================\n\n");

    printf("[Request Validation Tests]\n");
    test_validate_valid_request();
    test_validate_request_with_params();
    test_validate_missing_fields();
    test_validate_wrong_version();
    test_validate_wrong_field_types();
    printf("\n");

    printf("[Success Response Tests]\n");
    test_create_success_response();
    test_create_success_response_null_result();
    printf("\n");

    printf("[Error Response Tests]\n");
    test_create_error_response();
    test_create_error_response_with_data();
    test_create_parse_error_response();
    test_create_invalid_request_response();
    test_create_method_not_found_response();
    test_create_invalid_params_response();
    test_create_internal_error_response();
    test_create_rate_limited_response();
    test_create_auth_failed_response();
    printf("\n");

    printf("[Error Message Tests]\n");
    test_get_error_message();
    printf("\n");

    printf("[Boundary Condition Tests]\n");
    test_null_input_handling();
    test_memory_allocation_failure();
    printf("\n");

    printf("========================================\n");
    printf("  Results: %d/%d passed\n", g_tests_passed, g_tests_run);
    printf("========================================\n\n");

    airy_err_clear();

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
