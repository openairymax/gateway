// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gateway_rpc_handler.c
  * @brief 统一RPC处理模块单元测试
 *
 * 测试 gateway_rpc_handler 模块的核心功能：
  * - 请求验证和格式检查
  * - 字段提取和处理
  * - 错误处理机制
 * - 内存管理安全性
 *
  * Design principles:
 *   E-8 可测试性：覆盖率目标≥95%
  *   K-2 接口契约化：验证所有接口契约
 *
 */

// @owner: team-B
#include "gateway_rpc_handler.h"
#include "jsonrpc.h"
#include "airy_memory.h"
#include "error.h"
#include "error.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson_helpers.h>

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
#define ASSERT_NEQ(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

/**
  * @brief Create a standard JSON-RPC request
 */
static cJSON *create_valid_request(const char *method, int id_val)
{
    cJSON *request = cJSON_CreateObject();
    if (!request)
        return NULL;

    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddNumberToObject(request, "id", id_val);

    return request;
}

/**
  * @brief Create a JSON-RPC request with parameters
 */
static cJSON *create_request_with_params(const char *method, cJSON *params, int id_val)
{
    cJSON *request = cJSON_CreateObject();
    if (!request)
        return NULL;

    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", method);
    if (params) {
        cJSON_AddItemToObject(request, "params", params);
    }
    cJSON_AddNumberToObject(request, "id", id_val);

    return request;
}

/**
  * @brief Test valid request handling
 */
static void test_handle_valid_request(void)
{
    TEST_BEGIN("handle_valid_request");

    cJSON *request = create_valid_request("airy_sys_task_submit", 1);
    ASSERT_NOT_NULL(request);

    rpc_result_t result = gateway_rpc_handle_request(request, NULL, NULL);

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_EQ(result.error_code, 0);

    CJSON_PARSE_GUARD(resp, result.response_json, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *jsonrpc = cJSON_GetObjectItem(resp, "jsonrpc");
    ASSERT_NOT_NULL(jsonrpc);
    ASSERT_STR_EQ(jsonrpc->valuestring, "2.0");

    gateway_rpc_free(&result);
    cJSON_Delete(request);

    TEST_PASS();
}

/**
  * @brief Test a valid request with parameters
 */
static void test_handle_request_with_params(void)
{
    TEST_BEGIN("handle_request_with_params");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "test_task");

    cJSON *request = create_request_with_params("airy_sys_memory_write", params, 2);
    ASSERT_NOT_NULL(request);

    rpc_result_t result = gateway_rpc_handle_request(request, NULL, NULL);

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_EQ(result.error_code, 0);

    gateway_rpc_free(&result);
    cJSON_Delete(request);

    TEST_PASS();
}

/**
  * @brief Test NULL requests
 */
static void test_handle_null_request(void)
{
    TEST_BEGIN("handle_null_request");

    rpc_result_t result = gateway_rpc_handle_request(NULL, NULL, NULL);

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_NEQ(result.error_code, 0);

    CJSON_PARSE_GUARD(resp, result.response_json, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *error = cJSON_GetObjectItem(resp, "error");
    ASSERT_NOT_NULL(error);

    cJSON *code = cJSON_GetObjectItem(error, "code");
    ASSERT_NOT_NULL(code);
    ASSERT_EQ(code->valueint, -32600);

    gateway_rpc_free(&result);

    TEST_PASS();
}

/**
  * @brief Test invalid request formats
 */
static void test_handle_invalid_format(void)
{
    TEST_BEGIN("handle_invalid_format");

    cJSON *bad_request = cJSON_CreateObject();
    cJSON_AddStringToObject(bad_request, "method", "test");

    rpc_result_t result = gateway_rpc_handle_request(bad_request, NULL, NULL);

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_NEQ(result.error_code, 0);

    gateway_rpc_free(&result);
    cJSON_Delete(bad_request);

    TEST_PASS();
}

/**
  * @brief Test an invalid jsonrpc version
 */
static void test_handle_wrong_version(void)
{
    TEST_BEGIN("handle_wrong_version");

    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "1.0");
    cJSON_AddStringToObject(request, "method", "test");
    cJSON_AddNumberToObject(request, "id", 1);

    rpc_result_t result = gateway_rpc_handle_request(request, NULL, NULL);

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_NEQ(result.error_code, 0);

    gateway_rpc_free(&result);
    cJSON_Delete(request);

    TEST_PASS();
}

/**
  * @brief Example custom handler callback
 */
static int mock_handler(const char *request_str, char **response_str, void *user_data)
{
    (void)user_data;

    if (!request_str || !response_str)
        return AIRY_ERR_NULL_POINTER;

    CJSON_PARSE_GUARD(request, request_str, { return AIRY_ERR_PARSE_ERROR; });

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(response, "id", 999);
    cJSON_AddStringToObject(response, "result", "mock_handler_success");

    *response_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    return 0;
}

/**
  * @brief Test custom handler invocation
 */
static void test_custom_handler_invocation(void)
{
    TEST_BEGIN("custom_handler_invocation");

    cJSON *request = create_valid_request("custom_method", 10);
    ASSERT_NOT_NULL(request);

    rpc_result_t result = gateway_rpc_handle_request(request, mock_handler, NULL);

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_EQ(result.error_code, 0);

    CJSON_PARSE_GUARD(resp, result.response_json, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *result_field = cJSON_GetObjectItem(resp, "result");
    ASSERT_NOT_NULL(result_field);
    ASSERT_STR_EQ(result_field->valuestring, "mock_handler_success");

    gateway_rpc_free(&result);
    cJSON_Delete(request);

    TEST_PASS();
}

/**
  * @brief Error handler callback
 */
static int error_handler_func(const char *req, char **resp, void *data)
{
    (void)req;
    (void)resp;
    (void)data;
    return AIRY_ERR_UNKNOWN;
}

/**
  * @brief Test handler error returns
 */
static void test_custom_handler_error(void)
{
    TEST_BEGIN("custom_handler_error");

    cJSON *request = create_valid_request("test_method", 11);
    ASSERT_NOT_NULL(request);

    rpc_result_t result = gateway_rpc_handle_request(request, error_handler_func, NULL);

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_NEQ(result.error_code, 0);

    gateway_rpc_free(&result);
    cJSON_Delete(request);

    TEST_PASS();
}

/**
  * @brief Test error result creation
 */
static void test_create_error_result(void)
{
    TEST_BEGIN("create_error_result");

    rpc_result_t result = gateway_rpc_create_error(-32601, "Method not found");

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_EQ(result.error_code, -32601);

    CJSON_PARSE_GUARD(resp, result.response_json, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *error = cJSON_GetObjectItem(resp, "error");
    ASSERT_NOT_NULL(error);

    cJSON *code = cJSON_GetObjectItem(error, "code");
    ASSERT_NOT_NULL(code);
    ASSERT_EQ(code->valueint, -32601);

    cJSON *message = cJSON_GetObjectItem(error, "message");
    ASSERT_NOT_NULL(message);
    ASSERT_STR_EQ(message->valuestring, "Method not found");

    gateway_rpc_free(&result);

    TEST_PASS();
}

/**
  * @brief Test error creation with a NULL message
 */
static void test_create_error_null_message(void)
{
    TEST_BEGIN("create_error_null_message");

    rpc_result_t result = gateway_rpc_create_error(-32000, NULL);

    ASSERT_NOT_NULL(result.response_json);
    ASSERT_EQ(result.error_code, -32000);

    gateway_rpc_free(&result);

    TEST_PASS();
}

/**
  * @brief Test resource-free safety
 */
static void test_resource_cleanup_safety(void)
{
    TEST_BEGIN("resource_cleanup_safety");

    rpc_result_t null_result = {NULL, 0, NULL};
    gateway_rpc_free(&null_result);

    rpc_result_t result = gateway_rpc_create_error(-32000, "test");
    gateway_rpc_free(&result);
    gateway_rpc_free(&result);

    TEST_PASS();
}

/**
  * @brief Test memory stability under many requests
 */
static void test_memory_stability_under_load(void)
{
    TEST_BEGIN("memory_stability_under_load");

    for (int i = 0; i < 1000; i++) {
        char method[64];
        snprintf(method, sizeof(method), "airy_sys_task_submit_%d", i);

        cJSON *request = create_valid_request(method, i);
        ASSERT_NOT_NULL(request);

        rpc_result_t result = gateway_rpc_handle_request(request, NULL, NULL);

        if (result.response_json) {
            AIRY_FREE(result.response_json);
        }

        cJSON_Delete(request);
    }

    TEST_PASS();
}

/**
  * @brief Test an empty method name
 */
static void test_empty_method_name(void)
{
    TEST_BEGIN("empty_method_name");

    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", "");
    cJSON_AddNumberToObject(request, "id", 100);

    rpc_result_t result = gateway_rpc_handle_request(request, NULL, NULL);

    ASSERT_NOT_NULL(result.response_json);

    gateway_rpc_free(&result);
    cJSON_Delete(request);

    TEST_PASS();
}

/**
  * @brief Test an over-long method name
 */
static void test_very_long_method_name(void)
{
    TEST_BEGIN("very_long_method_name");

    char long_method[1024];
    AIRY_MEMSET(long_method, 'A', sizeof(long_method) - 1);
    long_method[sizeof(long_method) - 1] = '\0';

    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", long_method);
    cJSON_AddNumberToObject(request, "id", 101);

    rpc_result_t result = gateway_rpc_handle_request(request, NULL, NULL);

    ASSERT_NOT_NULL(result.response_json);

    gateway_rpc_free(&result);
    cJSON_Delete(request);

    TEST_PASS();
}

/**
  * @brief Test a method name with special characters
 */
static void test_special_characters_in_method(void)
{
    TEST_BEGIN("special_characters_in_method");

    const char *special_methods[] = {"agentrt.sys.task.submit", "agentrt/sys/task/submit",
                                     "agentrt::sys::task::submit", "airy_sys_task_submit_中文",
                                     NULL};

    for (int i = 0; special_methods[i] != NULL; i++) {
        cJSON *request = create_valid_request(special_methods[i], 102 + i);
        ASSERT_NOT_NULL(request);

        rpc_result_t result = gateway_rpc_handle_request(request, NULL, NULL);

        ASSERT_NOT_NULL(result.response_json);

        gateway_rpc_free(&result);
        cJSON_Delete(request);
    }

    TEST_PASS();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n======================================================\n");
    printf("  Gateway RPC Handler Unit Tests v1.0\n");
    printf("  (Testing unified RPC processing module)\n");
    printf("======================================================\n\n");

    printf("[Request Validation Tests]\n");
    test_handle_valid_request();
    test_handle_request_with_params();
    test_handle_null_request();
    test_handle_invalid_format();
    test_handle_wrong_version();
    printf("\n");

    printf("[Custom Handler Tests]\n");
    test_custom_handler_invocation();
    test_custom_handler_error();
    printf("\n");

    printf("[Error Handling Tests]\n");
    test_create_error_result();
    test_create_error_null_message();
    printf("\n");

    printf("[Memory Management Tests]\n");
    test_resource_cleanup_safety();
    test_memory_stability_under_load();
    printf("\n");

    printf("[Boundary Condition Tests]\n");
    test_empty_method_name();
    test_very_long_method_name();
    test_special_characters_in_method();
    printf("\n");

    printf("======================================================\n");
    printf("  Results: %d/%d passed\n", g_tests_passed, g_tests_run);
    printf("======================================================\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
