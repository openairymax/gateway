// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_syscall_router.c
 * @brief syscall_router 模块单元测试
 *
  * 测试系统调用路由器的核心功能：
  * - 方法名路由到正确的 syscall
  * - 参数验证和错误处理
  * - 响应生成正确性
 *
  * Design principles:
 *   E-8 可测试性：单元测试覆盖率≥80%
  *   K-2 接口契约化：验证路由契约
 *
 */

// @owner: team-B
#include "jsonrpc.h"
#include "syscall_router.h"

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
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

static cJSON *__attribute__((used)) create_jsonrpc_request(const char *method)
{
    cJSON *request = cJSON_CreateObject();
    if (!request)
        return NULL;

    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddNumberToObject(request, "id", 1);

    return request;
}

static cJSON *create_jsonrpc_request_with_params(const char *method, cJSON *params)
{
    cJSON *request = cJSON_CreateObject();
    if (!request)
        return NULL;

    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddItemToObject(request, "params", params);
    cJSON_AddNumberToObject(request, "id", 1);

    return request;
}

/**
  * @brief Test routing of task-management methods
 */
static void test_route_task_methods(void)
{
    TEST_BEGIN("route_task_methods");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "test_task");
    cJSON *request = create_jsonrpc_request_with_params("airy_sys_task_submit", params);

    char *response =
        gateway_syscall_route("airy_sys_task_submit", params, (cJSON *)jsonrpc_get_id(request));
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *jsonrpc_ver = cJSON_GetObjectItem(resp_json, "jsonrpc");
    ASSERT_NOT_NULL(jsonrpc_ver);
    ASSERT_STR_EQ(jsonrpc_ver->valuestring, "2.0");

    cJSON *id = cJSON_GetObjectItem(resp_json, "id");
    ASSERT_NOT_NULL(id);
    ASSERT_EQ(id->valueint, 1);

    cJSON_free(response);

    cJSON_Delete(request);

    response = gateway_syscall_route("airy_sys_task_query", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_task_wait", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_task_cancel", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Test routing of memory-management methods
 */
static void test_route_memory_methods(void)
{
    TEST_BEGIN("route_memory_methods");

    char *response;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "key", "test_key");
    cJSON_AddStringToObject(params, "value", "test_value");
    response = gateway_syscall_route("airy_sys_memory_write", params, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);
    cJSON_Delete(params);

    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "query", "search_query");
    response = gateway_syscall_route("airy_sys_memory_search", params, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);
    cJSON_Delete(params);

    response = gateway_syscall_route("airy_sys_memory_get", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_memory_delete", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Test routing of session-management methods
 */
static void test_route_session_methods(void)
{
    TEST_BEGIN("route_session_methods");

    char *response;

    response = gateway_syscall_route("airy_sys_session_create", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_session_get", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_session_close", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_session_list", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Test routing of observability methods
 */
static void test_route_telemetry_methods(void)
{
    TEST_BEGIN("route_telemetry_methods");

    char *response;

    response = gateway_syscall_route("airy_sys_telemetry_metrics", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_telemetry_traces", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Test routing of Agent-management methods
 */
static void test_route_agent_methods(void)
{
    TEST_BEGIN("route_agent_methods");

    char *response;

    response = gateway_syscall_route("airy_sys_agent_spawn", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_agent_terminate", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_agent_invoke", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("airy_sys_agent_list", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Test error handling for unknown method names
 */
static void test_route_unknown_method(void)
{
    TEST_BEGIN("route_unknown_method");

    char *response = gateway_syscall_route("airy_sys_unknown_method", NULL, NULL);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    ASSERT_NOT_NULL(error);

    cJSON *code = cJSON_GetObjectItem(error, "code");
    ASSERT_NOT_NULL(code);
    ASSERT_EQ(code->valueint, JSONRPC_METHOD_NOT_FOUND);

    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Verify syscall_router safety against NULL arguments
 */
static void test_null_safety(void)
{
    TEST_BEGIN("null_parameter_safety");

    char *response = gateway_syscall_route(NULL, NULL, NULL);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    ASSERT_NOT_NULL(error);

    cJSON_free(response);

    TEST_PASS();
}

/**
  * @brief Test method-name prefix matching
 */
static void test_method_prefix_matching(void)
{
    TEST_BEGIN("method_prefix_matching");

    char *response = gateway_syscall_route("airy_sys_task_submit", NULL, NULL);
    ASSERT_NOT_NULL(response);
    cJSON_free(response);

    response = gateway_syscall_route("task_submit", NULL, NULL);
    ASSERT_NOT_NULL(response);

    CJSON_PARSE_GUARD(resp_json, response, {
        TEST_FAIL("parse response failed");
        return;
    });

    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    ASSERT_NOT_NULL(error);

    cJSON_free(response);

    TEST_PASS();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n========================================\n");
    printf("  Syscall Router Unit Tests v1.0\n");
    printf("  (Testing 18 syscall methods)\n");
    printf("========================================\n\n");

    printf("[Task Management Tests]\n");
    test_route_task_methods();
    printf("\n");

    printf("[Memory Management Tests]\n");
    test_route_memory_methods();
    printf("\n");

    printf("[Session Management Tests]\n");
    test_route_session_methods();
    printf("\n");

    printf("[Telemetry Tests]\n");
    test_route_telemetry_methods();
    printf("\n");

    printf("[Agent Management Tests]\n");
    test_route_agent_methods();
    printf("\n");

    printf("[Error Handling Tests]\n");
    test_route_unknown_method();
    test_null_safety();
    test_method_prefix_matching();
    printf("\n");

    printf("========================================\n");
    printf("  Results: %d/%d passed\n", g_tests_passed, g_tests_run);
    printf("========================================\n\n");

    airy_err_clear();

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
