// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_svcdispatch_e2e.c
 * @brief E2E-1：svcdispatch 注入驱动的 gateway 翻译层端到端测试。
 *
 * 全链路（进程内跨层）：airy_sys_svc_call()（SYS_SVC_CALL 系统调用）
 *   -> gw_sys_svc_dispatch_init() 注入的派发钩子（gateway_biz_svcdispatch.c）
 *   -> ns -> daemon 端点映射（含 legacy ns 整编 plugin->tool / info->monit）
 *   -> gw_svc_call()（gateway_biz_forward.c，Unix socket JSON-RPC 客户端）
 *   -> 本测试内置 mock daemon（真实 accept/recv/send/close 往返）
 *   -> 完整 JSON-RPC 响应原文回传 *out_result。
 *
 * 架构判据：架构约束 2026-08-25 "必须走 syscall"；响应语义为完整响应
 * 原文（gw_svc_call 返回累积缓冲整体，gw_sys_svc_dispatch 原样透传）。
 *
 * mock 端点用 "<ns>.srvc" 非 .sock 后缀（daemon_l2_channel_for_socket 对
 * 非 .sock 后缀 fail-closed），锁定本测试只覆盖 socket 翻译路径；corekern
 * L2 灰度先行路由 test_gw_daemon_roundtrip.c 覆盖。
 *
 * socket 位于 /tmp/airy_svcdispatch_e2e_<pid>/（pid 后缀，ctest -j 并行安全）。
 */

// @owner: team-B

#include "gateway_biz_internal.h"

#include "svc_model_defaults.h"
#include "syscalls.h"

#include <cjson/cJSON.h>

#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <errno.h>
#include <stdatomic.h>
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
#define ASSERT_STREQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

/* ---- mock daemon：单线程 accept 循环，逐请求回显 JSON-RPC ---- */

#define MOCK_RESP_TIMEOUT_S 5

static char g_mock_sock[256];
static int g_mock_listen_fd = -1;
static _Atomic int g_mock_running = 0;
static pthread_t g_mock_thread;
static pthread_mutex_t g_mock_mtx = PTHREAD_MUTEX_INITIALIZER;

static char g_mock_last_method[128];
static char g_mock_last_params[1024];
static _Atomic int g_mock_last_id = -1;
static _Atomic int g_mock_hits = 0;

static void mock_handle_conn(int fd)
{
    struct timeval tv = {MOCK_RESP_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* gw_svc_call send 不带 NUL：拼包至 cJSON_Parse 成功即为一个完整请求 */
    char buf[8192];
    size_t used = 0;
    cJSON *root = NULL;
    while (!root) {
        if (used + 1 >= sizeof(buf))
            break;
        ssize_t n = recv(fd, buf + used, sizeof(buf) - used - 1, 0);
        if (n <= 0)
            break;
        used += (size_t)n;
        buf[used] = '\0';
        root = cJSON_Parse(buf);
    }
    if (!root)
        return;

    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *id = cJSON_GetObjectItem(root, "id");

    char *params_str = params ? cJSON_PrintUnformatted(params) : NULL;
    pthread_mutex_lock(&g_mock_mtx);
    memset(g_mock_last_method, 0, sizeof(g_mock_last_method));
    memset(g_mock_last_params, 0, sizeof(g_mock_last_params));
    if (cJSON_IsString(method) && method->valuestring)
        snprintf(g_mock_last_method, sizeof(g_mock_last_method), "%s", method->valuestring);
    if (params_str)
        snprintf(g_mock_last_params, sizeof(g_mock_last_params), "%s", params_str);
    g_mock_last_id = cJSON_IsNumber(id) ? (int)id->valuedouble : -1;
    g_mock_hits++;
    int req_id = g_mock_last_id;
    char wire_method[128];
    snprintf(wire_method, sizeof(wire_method), "%s", g_mock_last_method);
    pthread_mutex_unlock(&g_mock_mtx);
    free(params_str);

    /* 回显响应：result.ok + result.method（wire 名）+ result.params（原样回显） */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(resp, "id", req_id);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    cJSON_AddStringToObject(result, "method", wire_method);
    cJSON_AddItemToObject(result, "params",
                          params ? cJSON_Duplicate(params, 1) : cJSON_CreateObject());
    cJSON_AddItemToObject(resp, "result", result);

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!out) {
        cJSON_Delete(root);
        return;
    }
    size_t len = strlen(out);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, out + sent, len - sent, 0);
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
    free(out);
    cJSON_Delete(root);
    /* 回完即 close：gw_svc_call recv 循环 n<=0 break，EOF 即完整响应边界 */
    close(fd);
}

static void *mock_daemon_thread(void *arg)
{
    (void)arg;
    while (g_mock_running) {
        int fd = accept(g_mock_listen_fd, NULL, NULL);
        if (fd < 0)
            break;
        mock_handle_conn(fd);
    }
    return NULL;
}

static int mock_start(void)
{
    g_mock_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_mock_listen_fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_mock_sock);
    unlink(g_mock_sock);
    if (bind(g_mock_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(g_mock_listen_fd);
        g_mock_listen_fd = -1;
        return -1;
    }
    if (listen(g_mock_listen_fd, 8) != 0) {
        close(g_mock_listen_fd);
        g_mock_listen_fd = -1;
        return -1;
    }
    g_mock_running = 1;
    if (pthread_create(&g_mock_thread, NULL, mock_daemon_thread, NULL) != 0) {
        g_mock_running = 0;
        close(g_mock_listen_fd);
        g_mock_listen_fd = -1;
        return -1;
    }
    return 0;
}

static void mock_stop(void)
{
    g_mock_running = 0;
    if (g_mock_listen_fd >= 0) {
        /* Linux 下 close() 不保证唤醒阻塞在 accept() 的线程，须先 shutdown */
        shutdown(g_mock_listen_fd, SHUT_RDWR);
        close(g_mock_listen_fd); /* 唤醒阻塞在 accept 的线程 */
        g_mock_listen_fd = -1;
    }
    pthread_join(g_mock_thread, NULL);
    unlink(g_mock_sock);
}

static void mock_reset(void)
{
    pthread_mutex_lock(&g_mock_mtx);
    g_mock_last_method[0] = '\0';
    g_mock_last_params[0] = '\0';
    g_mock_last_id = -1;
    g_mock_hits = 0;
    pthread_mutex_unlock(&g_mock_mtx);
}

/* 快照最近一次请求记录（method/params/id），成功返回 0 */
static int mock_last(char *method, size_t msz, char *params, size_t psz, int *id, int *hits)
{
    pthread_mutex_lock(&g_mock_mtx);
    snprintf(method, msz, "%s", g_mock_last_method);
    snprintf(params, psz, "%s", g_mock_last_params);
    *id = g_mock_last_id;
    *hits = g_mock_hits;
    pthread_mutex_unlock(&g_mock_mtx);
    return method[0] == '\0' ? -1 : 0;
}

/* ---- 公共调用辅助：airy_sys_svc_call + 结果快照 ---- */

static char g_path_llm[256];
static char g_path_tool[256];
static char g_path_monit[256];
static char g_path_sched[256];

/* 成功路径通用断言：rc==0、out 为完整响应原文、root->result 可解析 */
static void call_expect_ok(const char *ns, const char *method, const char *params_json,
                           const char *expect_wire_method)
{
    char *out = NULL;
    int rc = (int)airy_sys_svc_call(ns, method, params_json, 3000, &out);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(out);

    cJSON *root = cJSON_Parse(out);
    ASSERT_NOT_NULL(root); /* 完整响应原文：整体必须为合法 JSON */
    cJSON *result = cJSON_GetObjectItem(root, "result");
    ASSERT_NOT_NULL(result);
    cJSON *ok = cJSON_GetObjectItem(result, "ok");
    ASSERT_TRUE(cJSON_IsBool(ok) && cJSON_IsTrue(ok));

    char got_method[128];
    char got_params[1024];
    int got_id = -1;
    int hits = 0;
    ASSERT_EQ(mock_last(got_method, sizeof(got_method), got_params, sizeof(got_params), &got_id,
                        &hits),
              0);
    ASSERT_STREQ(got_method, expect_wire_method);
    ASSERT_EQ(got_id, 1); /* gw_svc_call 内部请求 id 恒为 1 */
    (void)hits;
    cJSON_Delete(root);
    AIRY_FREE(out);

    TEST_PASS();
}

/* 失败路径通用断言：rc!=0 且 out==NULL（错误不得产生半成品响应） */
static void call_expect_fail(const char *ns, const char *method, const char *params_json)
{
    char *out = NULL;
    int rc = (int)airy_sys_svc_call(ns, method, params_json, 3000, &out);
    ASSERT_TRUE(rc != 0);
    ASSERT_NULL(out);
    TEST_PASS();
}

/* ---- 用例 ---- */

static void test_llm_roundtrip(void)
{
    TEST_BEGIN("svc_call_llm_roundtrip");
    mock_reset();
    call_expect_ok("llm", "complete", "{\"prompt\":\"hi\"}", "complete");
}

static void test_ns_trailing_dot(void)
{
    TEST_BEGIN("svc_call_ns_trailing_dot");
    mock_reset();
    /* gw_svc_sock_for_ns：'llm.' 与 'llm' 等价（尾点剥离） */
    call_expect_ok("llm.", "ping", "{}", "ping");
}

static void test_params_passthrough(void)
{
    TEST_BEGIN("svc_call_params_passthrough");
    mock_reset();
    /* params 原样透传：嵌套对象/数组/数字/字符串往返一致 */
    const char *params = "{\"a\":1,\"b\":[2,3],\"c\":{\"d\":\"x\"}}";
    call_expect_ok("llm", "invoke", params, "invoke");

    char got_method[128];
    char got_params[1024];
    int got_id = -1;
    int hits = 0;
    ASSERT_EQ(mock_last(got_method, sizeof(got_method), got_params, sizeof(got_params), &got_id,
                        &hits),
              0);
    /* mock 回显 result.params 即收到原文；键序/紧凑格式由 cJSON 规范化保序 */
    ASSERT_STREQ(got_params, params);
}

static void test_legacy_plugin_prefix(void)
{
    TEST_BEGIN("svc_call_legacy_plugin_prefix");
    mock_reset();
    /* legacy 整编：plugin.* → tool 端点 + "plugin_" wire 前缀（l2_pass=0） */
    call_expect_ok("plugin", "load", "{\"name\":\"demo\"}", "plugin_load");
}

static void test_legacy_info_l2_pass(void)
{
    TEST_BEGIN("svc_call_legacy_info_l2_pass");
    mock_reset();
    /* legacy 整编：info.* → monit 端点；L2 三件套（health_check）透传不加前缀 */
    call_expect_ok("info", "health_check", "{}", "health_check");
}

static void test_legacy_info_prefix(void)
{
    TEST_BEGIN("svc_call_legacy_info_prefix");
    mock_reset();
    /* legacy 整编：info.* 非三件套 → "info_" wire 前缀 */
    call_expect_ok("info", "system", "{}", "info_system");
}

static void test_unknown_ns(void)
{
    TEST_BEGIN("svc_call_unknown_ns");
    mock_reset();
    call_expect_fail("bogus", "any", "{}");
}

static void test_unreachable_endpoint(void)
{
    TEST_BEGIN("svc_call_unreachable_endpoint");
    mock_reset();
    /* sched 端点指向不存在路径：connect 失败 → gw_svc_call NULL → rc!=0 */
    call_expect_fail("sched", "plan", "{}");
}

static void test_null_method(void)
{
    TEST_BEGIN("svc_call_null_method");
    mock_reset();
    call_expect_fail("llm", NULL, "{}");
}

static void test_after_cleanup(void)
{
    TEST_BEGIN("svc_call_after_cleanup");
    mock_reset();
    /* cleanup 已在 main 中执行：钩子清除 → syscall 层返回 EPROTONOSUPPORT，
     * out==NULL（无钩子 fail-closed，不落空指针） */
    char *out = NULL;
    int rc = (int)airy_sys_svc_call("llm", "complete", "{}", 3000, &out);
    ASSERT_EQ(rc, (int)AIRY_EPROTONOSUPPORT);
    ASSERT_NULL(out);
    TEST_PASS();
}

int main(void)
{
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/airy_svcdispatch_e2e_%d", (int)getpid());
    if (mkdir(tmpdir, 0700) != 0 && errno != EEXIST) {
        printf("FATAL: mkdir %s failed: %s\n", tmpdir, strerror(errno));
        return 1;
    }

    /* mock 端点：非 .sock 后缀（.srvc）锁定 socket 翻译路径（L2 fail-closed） */
    snprintf(g_mock_sock, sizeof(g_mock_sock), "%s/llm.srvc", tmpdir);
    snprintf(g_path_llm, sizeof(g_path_llm), "%s", g_mock_sock);
    snprintf(g_path_tool, sizeof(g_path_tool), "%s", g_mock_sock);
    snprintf(g_path_monit, sizeof(g_path_monit), "%s", g_mock_sock);
    /* sched 指向不存在路径：不可达分支（connect ENOENT） */
    snprintf(g_path_sched, sizeof(g_path_sched), "%s/no_such.srvc", tmpdir);

    if (mock_start() != 0) {
        printf("FATAL: mock daemon start failed\n");
        return 1;
    }

    /* 栈上业务上下文：端点映射与 gateway_d 宿主注入等价 */
    gateway_business_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.llm_sock_path, sizeof(ctx.llm_sock_path), "%s", g_path_llm);
    snprintf(ctx.tool_sock_path, sizeof(ctx.tool_sock_path), "%s", g_path_tool);
    snprintf(ctx.monit_sock_path, sizeof(ctx.monit_sock_path), "%s", g_path_monit);
    snprintf(ctx.sched_sock_path, sizeof(ctx.sched_sock_path), "%s", g_path_sched);
    snprintf(ctx.default_model, sizeof(ctx.default_model), "%s", SVC_MODEL_DEFAULT_FALLBACK);

    if (gw_sys_svc_dispatch_init(&ctx) != 0) {
        printf("FATAL: svc dispatch hook inject failed\n");
        mock_stop();
        return 1;
    }

    printf("== svcdispatch e2e (SYS_SVC_CALL -> hook -> gw_svc_call -> mock daemon) ==\n");

    test_llm_roundtrip();
    test_ns_trailing_dot();
    test_params_passthrough();
    test_legacy_plugin_prefix();
    test_legacy_info_l2_pass();
    test_legacy_info_prefix();
    test_unknown_ns();
    test_unreachable_endpoint();
    test_null_method();

    /* 清钩子后验证 syscall 层 fail-closed（第 10 用例） */
    gw_sys_svc_dispatch_cleanup();
    test_after_cleanup();

    mock_stop();

    printf("Results: %d/%d tests passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
