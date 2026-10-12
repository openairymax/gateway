// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gateway.c
 * @brief Gateway 模块单元测试（匹配当前公共 API）
 *
 * 测试网关模块的核心功能：
 * - 类型枚举与错误码契约
 * - 生命周期接口（start/destroy）
 * - NULL 安全验证（E-6 错误可追溯：非法输入不得崩溃）
 *
 * 2026-08-20：重写匹配当前 include/gateway.h 公共 API
 * （旧版 gateway_create/gateway_create_interface 已随网关重构移除，
 * 原测试因引用已删除符号被禁用）。
 * 2026-10-12：公共 API 收敛为 start/destroy 两操作，删除已裁断
 * 门面（stop/stats/set_handler/is_running/get_type/get_name/
 * register_endpoint/三 create）对应的测试段。
 */

// @owner: team-B
#include "gateway.h"
#include "error.h"

#include <stdio.h>
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
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

/**
 * @brief Verify the gateway type enum values
 */
static void test_gateway_types(void)
{
    TEST_BEGIN("gateway_type_enum_values");

    ASSERT_EQ(GATEWAY_TYPE_HTTP, 0);
    ASSERT_EQ(GATEWAY_TYPE_WS, 1);
    ASSERT_EQ(GATEWAY_TYPE_STDIO, 2);

    TEST_PASS();
}

/**
 * @brief Verify the gateway error code definitions
 */
static void test_error_codes(void)
{
    TEST_BEGIN("gateway_error_codes");

    ASSERT_EQ(GATEWAY_SUCCESS, 0);
    ASSERT_EQ(GATEWAY_ERROR_INVALID, -1);
    ASSERT_EQ(GATEWAY_ERROR_MEMORY, -2);
    ASSERT_EQ(GATEWAY_ERROR_IO, -3);
    ASSERT_EQ(GATEWAY_ERROR_TIMEOUT, -4);
    ASSERT_EQ(GATEWAY_ERROR_CLOSED, -5);
    ASSERT_EQ(GATEWAY_ERROR_PROTOCOL, -6);

    TEST_PASS();
}

/**
 * @brief Verify all public APIs are safe against NULL input
 *
 * Per E-6 error traceability: invalid input must not crash
 */
static void test_null_safety(void)
{
    TEST_BEGIN("null_pointer_safety");

    ASSERT_NE(gateway_start(NULL), AIRY_SUCCESS);
    gateway_destroy(NULL);

    TEST_PASS();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n========================================\n");
    printf("  Gateway Module Unit Tests v0.1.0\n");
    printf("  (Aligned with ARCHITECTURAL_PRINCIPLES)\n");
    printf("========================================\n\n");

    printf("[Definition Tests]\n");
    test_gateway_types();
    test_error_codes();
    printf("\n");

    printf("[Safety Tests]\n");
    test_null_safety();
    printf("\n");

    printf("========================================\n");
    printf("  Results: %d/%d passed\n", g_tests_passed, g_tests_run);
    printf("========================================\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
