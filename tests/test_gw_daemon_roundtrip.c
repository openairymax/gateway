// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gw_daemon_roundtrip.c
 * @brief E2E-2：gateway -> daemon 双 transport 往返（Blueprint 8.3.3）。
 *
 * 全链路：gw_svc_call()（gateway L2 协议客户端）按 ns 传输开关二选一：
 *   - socket 路径：Unix socket JSON-RPC -> mock daemon（真实 accept/往返）
 *   - corekern 先行路：daemon_l2_channel_for_socket（"<ns>.rpc" 派生 +
 *     AIRY_<NS>_IPC_TRANSPORT=corekern 开关 gate）-> daemon_l2_rpc_call_resp
 *     -> 本测试挂载的 L2 envelope bridge -> 回落语义验证
 *
 * 三个用例（8.3.3 灰度规范全矩阵）：
 *   A. 开关缺省（off）：channel_for_socket NOT_FOUND -> 纯 socket 往返
 *   B. 开关 on + bridge 挂载：L2 先行路成功，socket daemon 零触达
 *   C. bridge 卸载（开关仍 on）：ENOENT -> 回落 socket 路径（灰度共存常态）
 *
 * socket 位于 /tmp/airy_gw_roundtrip_<pid>/（pid 后缀，ctest -j 并行安全）。
 * mock socket 名 "<ns>.sock"（channel 派生要求 .sock 后缀）。
 */

// @owner: team-B

#include "gateway_biz_internal.h"
#include "daemon_l1_server.h"

#include "ipc.h"

#include <cjson/cJSON.h>

#include "airy_memory.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

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

#define ASSERT_NULL(ptr) ASSERT_TRUE((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT_TRUE((ptr) != NULL)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_STREQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

/* ---- mock daemon（两实例：transport off 演练 + fallback 演练）---- */

struct mock_daemon {
    char sock[256];
    int listen_fd;
    _Atomic int running;
    pthread_t thread;
    pthread_mutex_t mtx;
    _Atomic int hits;
    char last_method[128];
};

static struct mock_daemon g_mock_a;
static struct mock_daemon g_mock_b;
static daemon_l2_bridge_t *g_bridge = NULL; /* Case B 挂载，Case C 卸载 */

static void mock_handle_conn(struct mock_daemon *m, int fd)
{
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
    char wire_method[128] = {0};
    if (cJSON_IsString(method) && method->valuestring)
        snprintf(wire_method, sizeof(wire_method), "%s", method->valuestring);

    pthread_mutex_lock(&m->mtx);
    snprintf(m->last_method, sizeof(m->last_method), "%s", wire_method);
    int req_id = ++m->hits;
    pthread_mutex_unlock(&m->mtx);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(resp, "id", 1);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "via", "socket");
    cJSON_AddStringToObject(result, "method", wire_method);
    cJSON_AddNumberToObject(result, "hits", req_id);
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
    /* 回完即 close：gw_svc_call recv 循环以 EOF 为响应边界 */
    close(fd);
}

static void *mock_thread_fn(void *arg)
{
    struct mock_daemon *m = (struct mock_daemon *)arg;
    while (m->running) {
        int fd = accept(m->listen_fd, NULL, NULL);
        if (fd < 0)
            break;
        mock_handle_conn(m, fd);
    }
    return NULL;
}

static int mock_start(struct mock_daemon *m, const char *sock_path)
{
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->mtx, NULL);
    snprintf(m->sock, sizeof(m->sock), "%s", sock_path);
    m->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m->listen_fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", m->sock);
    unlink(m->sock);
    if (bind(m->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(m->listen_fd, 8) != 0) {
        close(m->listen_fd);
        m->listen_fd = -1;
        return -1;
    }
    m->running = 1;
    if (pthread_create(&m->thread, NULL, mock_thread_fn, m) != 0) {
        m->running = 0;
        close(m->listen_fd);
        m->listen_fd = -1;
        return -1;
    }
    return 0;
}

static void mock_stop(struct mock_daemon *m)
{
    m->running = 0;
    if (m->listen_fd >= 0) {
        /* Linux 下 close() 不保证唤醒阻塞在 accept() 的线程，须先 shutdown */
        shutdown(m->listen_fd, SHUT_RDWR);
        close(m->listen_fd); /* 唤醒阻塞在 accept 的线程 */
    }
    /* join 建立同步边沿后再置 -1，避免与 accept(m->listen_fd) 竞态 */
    pthread_join(m->thread, NULL);
    m->listen_fd = -1;
    unlink(m->sock);
    pthread_mutex_destroy(&m->mtx);
}

static void mock_reset(struct mock_daemon *m)
{
    pthread_mutex_lock(&m->mtx);
    m->hits = 0;
    m->last_method[0] = '\0';
    pthread_mutex_unlock(&m->mtx);
}

/* ---- L2 bridge dispatch：bridge 线程上被调用，记录请求并回固定响应 ---- */

static pthread_mutex_t g_l2_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_l2_hits = 0;
static char g_l2_last_method[128];

static int l2_dispatch(const char *req_json, size_t req_len, char **resp_json,
                       size_t *resp_len, void *userdata)
{
    (void)userdata;
    /* req 无 NUL 保证：拷贝补 NUL 再解析 */
    char buf[4096];
    size_t n = req_len < sizeof(buf) - 1 ? req_len : sizeof(buf) - 1;
    memcpy(buf, req_json, n);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
        return -1; /* 非零 -> sender 收 CANCELED */
    cJSON *method = cJSON_GetObjectItem(root, "method");
    pthread_mutex_lock(&g_l2_mtx);
    g_l2_hits++;
    g_l2_last_method[0] = '\0';
    if (cJSON_IsString(method) && method->valuestring)
        snprintf(g_l2_last_method, sizeof(g_l2_last_method), "%s", method->valuestring);
    pthread_mutex_unlock(&g_l2_mtx);
    cJSON_Delete(root);

    /* 完整 JSON-RPC 响应（daemon_l2_rpc_call_resp 原样透传给 gw_svc_call） */
    static const char resp[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"via\":\"l2\"}}";
    char *out = (char *)AIRY_MALLOC(sizeof(resp));
    if (!out)
        return -1;
    memcpy(out, resp, sizeof(resp));
    *resp_json = out;
    *resp_len = sizeof(resp) - 1; /* wire 长度不含 NUL；发送端自行补 NUL */
    return 0;
}

/* ---- 断言辅助：解析 gw_svc_call 完整响应原文中的 result.via ---- */

static cJSON *call_parse_result(const char *sock_path, const char *method)
{
    char *resp = gw_svc_call(sock_path, method, "{}", 3000);
    if (!resp) {
        TEST_FAIL("gw_svc_call: NULL response");
        return NULL;
    }
    cJSON *root = cJSON_Parse(resp); /* 完整响应原文：整体必须为合法 JSON */
    AIRY_FREE(resp);
    if (!root) {
        TEST_FAIL("gw_svc_call: response is not valid JSON");
        return NULL;
    }
    if (!cJSON_GetObjectItem(root, "result")) {
        TEST_FAIL("gw_svc_call: response lacks result");
        cJSON_Delete(root);
        return NULL;
    }
    return root; /* caller: cJSON_Delete(root) */
}

static const char *result_via(const cJSON *root)
{
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *via = cJSON_GetObjectItem(result, "via");
    return cJSON_IsString(via) ? via->valuestring : NULL;
}

static int mock_hits(struct mock_daemon *m)
{
    pthread_mutex_lock(&m->mtx);
    int h = m->hits;
    pthread_mutex_unlock(&m->mtx);
    return h;
}

/* ---- 用例 ---- */

/* Case A：AIRY_E2EA_IPC_TRANSPORT 未设 -> channel 派生 NOT_FOUND -> 纯 socket */
static void test_case_a_transport_off_socket_path(void)
{
    TEST_BEGIN("case_a_transport_off_socket_path");

    mock_reset(&g_mock_a);
    mock_reset(&g_mock_b);
    pthread_mutex_lock(&g_l2_mtx);
    g_l2_hits = 0;
    pthread_mutex_unlock(&g_l2_mtx);

    cJSON *root = call_parse_result(g_mock_a.sock, "ping");
    ASSERT_NOT_NULL(root);
    ASSERT_STREQ(result_via(root), "socket");
    cJSON_Delete(root);

    ASSERT_EQ(mock_hits(&g_mock_a), 1);
    ASSERT_EQ(mock_hits(&g_mock_b), 0);
    pthread_mutex_lock(&g_l2_mtx);
    ASSERT_EQ(g_l2_hits, 0);
    pthread_mutex_unlock(&g_l2_mtx);

    TEST_PASS();
}

/* Case B：开关 on + bridge 挂载 -> L2 先行路成功，socket daemon 零触达 */
static void test_case_b_corekern_l2_first_path(void)
{
    TEST_BEGIN("case_b_corekern_l2_first_path");

    ASSERT_EQ(setenv("AIRY_GWSCE2E_IPC_TRANSPORT", "corekern", 1), 0);

    g_bridge = daemon_l2_bridge_start("gwsce2e.rpc", l2_dispatch, NULL);
    ASSERT_NOT_NULL(g_bridge); /* bridge_start 自举 corekern IPC */

    mock_reset(&g_mock_b);
    cJSON *root = call_parse_result(g_mock_b.sock, "l2ping");
    ASSERT_NOT_NULL(root);
    ASSERT_STREQ(result_via(root), "l2");
    cJSON_Delete(root);

    pthread_mutex_lock(&g_l2_mtx);
    ASSERT_EQ(g_l2_hits, 1);
    ASSERT_STREQ(g_l2_last_method, "l2ping");
    pthread_mutex_unlock(&g_l2_mtx);
    ASSERT_EQ(mock_hits(&g_mock_b), 0);

    TEST_PASS();
    /* bridge 句柄保留在 g_bridge，由 main 在 Case C 前卸载验证回落语义 */
}

/* Case C：bridge 卸载（开关仍 on）-> L2 ENOENT -> 回落 socket（灰度共存） */
static void test_case_c_bridge_unmount_fallback(void)
{
    TEST_BEGIN("case_c_bridge_unmount_fallback");

    daemon_l2_bridge_stop(NULL); /* NULL 安全空操作 */
    /* Case B 挂载的 bridge 已由 main 卸载后再进入本用例 */

    mock_reset(&g_mock_a);
    mock_reset(&g_mock_b);
    cJSON *root = call_parse_result(g_mock_b.sock, "fall");
    ASSERT_NOT_NULL(root);
    ASSERT_STREQ(result_via(root), "socket");
    cJSON_Delete(root);

    ASSERT_EQ(mock_hits(&g_mock_b), 1);
    pthread_mutex_lock(&g_l2_mtx);
    ASSERT_EQ(g_l2_hits, 1); /* L2 不再被触达 */
    pthread_mutex_unlock(&g_l2_mtx);
    ASSERT_EQ(mock_hits(&g_mock_a), 0);

    TEST_PASS();
}

int main(void)
{
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/airy_gw_roundtrip_%d", (int)getpid());
    if (mkdir(tmpdir, 0700) != 0 && errno != EEXIST) {
        printf("FATAL: mkdir %s failed\n", tmpdir);
        return 1;
    }

    char sock_a[256];
    char sock_b[256];
    snprintf(sock_a, sizeof(sock_a), "%s/e2ea.sock", tmpdir);
    snprintf(sock_b, sizeof(sock_b), "%s/gwsce2e.sock", tmpdir);
    if (mock_start(&g_mock_a, sock_a) != 0 || mock_start(&g_mock_b, sock_b) != 0) {
        printf("FATAL: mock daemon start failed\n");
        mock_stop(&g_mock_a);
        mock_stop(&g_mock_b);
        return 1;
    }

    printf("== gw <-> daemon roundtrip (socket path / corekern L2 first path / fallback) ==\n");

    test_case_a_transport_off_socket_path();

    test_case_b_corekern_l2_first_path();
    /* 卸载 Case B 挂载的 bridge，Case C 验证 8.3.3 回落语义（灰度共存常态） */
    daemon_l2_bridge_stop(g_bridge);
    g_bridge = NULL;
    test_case_c_bridge_unmount_fallback();

    mock_stop(&g_mock_a);
    mock_stop(&g_mock_b);
    unsetenv("AIRY_GWSCE2E_IPC_TRANSPORT");
    airy_ipc_cleanup(); /* 既有 L2 测试生命周期惯例 */

    printf("Results: %d/%d tests passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
