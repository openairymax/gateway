// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B

/**
 * @file test_gw_aipc_client_e2e.c
 * @brief E2E-3（0.1.16 B3）：gateway 南向统一 A-IPC 客户端面往返。
 *
 * mock daemon（真实 UDS accept/往返）× 三个入口：
 *   A. gw_aipc_call：mock 分片回包（3 片，usleep 强制多次 recv）→
 *      gateway 聚合为完整 JSON-RPC 响应（EOF 定界）
 *   B. gw_aipc_stream：连接 + 请求下发 → mock 读到请求后分片回推 →
 *      调用方 fd 消费聚合（流式过渡态契约）
 *   C. gw_aipc_subscribe：握手字节原样到达 mock → 订阅帧回推可读
 *   D. 失败路径：端点不可达快速失败，不得悬挂
 *
 * 注意：客户端不发完请求即等响应（不关写侧），mock 读请求一律
 * 「增量 recv + 逐步解析/前缀判定」，禁止以 EOF 为请求边界（会死锁）。
 *
 * socket 位于 /tmp/airy_gw_aipc_<pid>/（pid 后缀，ctest -j 并行安全）。
 */

#include "gateway_aipc_client.h"

#include "airy_memory.h"

#include <cjson/cJSON.h>

#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <errno.h>
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

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_STREQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

/* ---- mock daemon：accept 线程 + 每连接 handler 回调 ---- */

struct mock_server {
    char sock[256];
    int listen_fd;
    volatile int running;
    pthread_t thread;
    void (*handler)(int fd, void *arg);
    void *arg;
};

/* 增量 recv 直至请求解析成功（客户端不关写侧：EOF 定界仅用于响应侧）。
 * 返回 0 成功（buf 以 NUL 结尾），-1 对端关闭/超限且始终未成完整 JSON。 */
static int mock_read_request(int fd, char *buf, size_t cap)
{
    size_t used = 0;
    buf[0] = '\0';
    for (;;) {
        if (used + 1 >= cap)
            return -1;
        ssize_t n = recv(fd, buf + used, cap - used - 1, 0);
        if (n <= 0)
            return -1;
        used += (size_t)n;
        buf[used] = '\0';
        cJSON *probe = cJSON_Parse(buf);
        if (probe) {
            cJSON_Delete(probe);
            return 0;
        }
    }
}

static void mock_send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0)
            return;
        sent += (size_t)n;
    }
}

static void *mock_thread_fn(void *arg)
{
    struct mock_server *m = (struct mock_server *)arg;
    while (m->running) {
        int fd = accept(m->listen_fd, NULL, NULL);
        if (fd < 0)
            break;
        m->handler(fd, m->arg);
        close(fd);
    }
    return NULL;
}

static int mock_start(struct mock_server *m, const char *sock_path,
                      void (*handler)(int, void *), void *arg)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->sock, sizeof(m->sock), "%s", sock_path);
    m->handler = handler;
    m->arg = arg;
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

static void mock_stop(struct mock_server *m)
{
    m->running = 0;
    if (m->listen_fd >= 0) {
        /* Linux 下 close() 不保证唤醒阻塞在 accept() 的线程，须先 shutdown */
        shutdown(m->listen_fd, SHUT_RDWR);
        close(m->listen_fd);
        m->listen_fd = -1;
    }
    pthread_join(m->thread, NULL);
    unlink(m->sock);
}

/* 测试 socket 路径（main 里按 pid 目录填充，ctest -j 并行安全） */
static char g_dir[256];
static char g_sock_call[320];
static char g_sock_stream[320];
static char g_sock_sub[320];
static char g_sock_missing[320];

/* ---- Case A：gw_aipc_call 分片回包聚合 ---- */

static void chunked_resp_handler(int fd, void *arg)
{
    (void)arg;
    char buf[4096];
    if (mock_read_request(fd, buf, sizeof(buf)) != 0)
        return;

    /* 完整响应分 3 片下发，片间 usleep 强制 gateway 侧多次 recv 聚合 */
    static const char resp[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"echo\":\"chunked-roundtrip\","
        "\"chunks\":3}}";
    size_t len = sizeof(resp) - 1;
    size_t third = len / 3;
    mock_send_all(fd, resp, third);
    usleep(20000);
    mock_send_all(fd, resp + third, third);
    usleep(20000);
    mock_send_all(fd, resp + 2 * third, len - 2 * third);
    /* 关闭连接 = 响应边界（EOF 定界） */
}

static void test_a_call_chunked_aggregation(void)
{
    TEST_BEGIN("case_a_call_chunked_aggregation");

    struct mock_server m;
    ASSERT_TRUE(mock_start(&m, g_sock_call, chunked_resp_handler, NULL) == 0);

    char *resp = gw_aipc_call(m.sock, "ping", "{\"k\":\"v\"}", 3000);
    ASSERT_TRUE(resp != NULL);

    cJSON *root = cJSON_Parse(resp); /* 聚合原文必须为合法 JSON */
    AIRY_FREE(resp);
    ASSERT_TRUE(root != NULL);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *echo = result ? cJSON_GetObjectItem(result, "echo") : NULL;
    ASSERT_TRUE(cJSON_IsString(echo) && echo->valuestring);
    ASSERT_STREQ(echo->valuestring, "chunked-roundtrip");
    cJSON *chunks = cJSON_GetObjectItem(result, "chunks");
    ASSERT_TRUE(cJSON_IsNumber(chunks) && chunks->valueint == 3);
    cJSON_Delete(root);

    mock_stop(&m);
    TEST_PASS();
}

/* ---- Case B：gw_aipc_stream 连接 + 下发，mock 分片回推 ---- */

struct stream_arg {
    char last_req[4096];
};

static void stream_handler(int fd, void *arg)
{
    struct stream_arg *sa = (struct stream_arg *)arg;
    if (mock_read_request(fd, sa->last_req, sizeof(sa->last_req)) != 0)
        return;
    /* 分 2 片回推流式数据（调用方 fd 消费） */
    mock_send_all(fd, "chunk-1;", 8);
    usleep(20000);
    mock_send_all(fd, "chunk-2", 7);
}

static void test_b_stream_open_and_ship(void)
{
    TEST_BEGIN("case_b_stream_open_and_ship");

    struct stream_arg sa;
    memset(&sa, 0, sizeof(sa));
    struct mock_server m;
    ASSERT_TRUE(mock_start(&m, g_sock_stream, stream_handler, &sa) == 0);

    int fd = gw_aipc_stream(m.sock, "{\"method\":\"agent.run_stream\"}", 0);
    ASSERT_TRUE(fd >= 0);

    /* mock 侧必须收到完整请求（连接 + 下发归一） */
    for (int i = 0; i < 100 && sa.last_req[0] == '\0'; i++)
        usleep(10000);
    ASSERT_STREQ(sa.last_req, "{\"method\":\"agent.run_stream\"}");

    /* 调用方 fd 消费：聚合 mock 分片回推 */
    char acc[256] = {0};
    size_t used = 0;
    for (;;) {
        char tmp[128];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > sizeof(acc))
            break;
        memcpy(acc + used, tmp, (size_t)n);
        used += (size_t)n;
        acc[used] = '\0';
    }
    close(fd);
    ASSERT_STREQ(acc, "chunk-1;chunk-2");

    mock_stop(&m);
    TEST_PASS();
}

/* ---- Case C：gw_aipc_subscribe 握手原样到达 + 订阅帧可读 ---- */

static void subscribe_handler(int fd, void *arg)
{
    (void)arg;
    /* 握手以 "\r\n\r\n" 收尾：增量 recv 至出现完整 HTTP 请求头 */
    char buf[2048];
    size_t used = 0;
    buf[0] = '\0';
    for (;;) {
        if (used + 1 >= sizeof(buf))
            return;
        ssize_t n = recv(fd, buf + used, sizeof(buf) - used - 1, 0);
        if (n <= 0)
            return;
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n"))
            break;
    }
    /* 验证订阅握手（HTTP SSE 请求）原样到达后再推一帧事件 */
    if (strstr(buf, "GET /events") && strstr(buf, "X-Client-Id: gateway-pep"))
        mock_send_all(fd, "data: {\"event\":\"epoch_change\"}\n\n", 33);
}

static void test_c_subscribe_handshake(void)
{
    TEST_BEGIN("case_c_subscribe_handshake");

    struct mock_server m;
    ASSERT_TRUE(mock_start(&m, g_sock_sub, subscribe_handler, NULL) == 0);

    const char *handshake = "GET /events HTTP/1.1\r\n"
                            "Accept: text/event-stream\r\n"
                            "X-Client-Id: gateway-pep\r\n"
                            "\r\n";
    int fd = gw_aipc_subscribe(m.sock, handshake, strlen(handshake));
    ASSERT_TRUE(fd >= 0);

    /* 订阅帧可读（帧下发以 mock close 为界） */
    char acc[256] = {0};
    size_t used = 0;
    for (;;) {
        char tmp[128];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > sizeof(acc))
            break;
        memcpy(acc + used, tmp, (size_t)n);
        used += (size_t)n;
        acc[used] = '\0';
    }
    close(fd);
    ASSERT_TRUE(strstr(acc, "epoch_change") != NULL);

    mock_stop(&m);
    TEST_PASS();
}

/* ---- Case D：失败路径（端点不可达）快速失败 ---- */

static void test_d_unreachable_fails_fast(void)
{
    TEST_BEGIN("case_d_unreachable_fails_fast");

    /* 不存在的 socket 路径：三个入口一律 -1/NULL，不得悬挂 */
    ASSERT_TRUE(gw_aipc_call(g_sock_missing, "ping", "{}", 200) == NULL);
    ASSERT_EQ(gw_aipc_stream(g_sock_missing, "{}", 0), -1);
    ASSERT_EQ(gw_aipc_subscribe(g_sock_missing, "GET /events\r\n\r\n", 14), -1);

    TEST_PASS();
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/airy_gw_aipc_%d", (int)getpid());
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) {
        printf("FATAL: mkdir %s failed\n", g_dir);
        return 1;
    }
    snprintf(g_sock_call, sizeof(g_sock_call), "%s/call.sock", g_dir);
    snprintf(g_sock_stream, sizeof(g_sock_stream), "%s/stream.sock", g_dir);
    snprintf(g_sock_sub, sizeof(g_sock_sub), "%s/sub.sock", g_dir);
    snprintf(g_sock_missing, sizeof(g_sock_missing), "%s/missing.sock", g_dir);

    printf("== gateway southbound A-IPC unified client face (B3 E2E) ==\n");

    test_a_call_chunked_aggregation();
    test_b_stream_open_and_ship();
    test_c_subscribe_handshake();
    test_d_unreachable_fails_fast();

    printf("Results: %d/%d tests passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
