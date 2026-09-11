// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gateway_auth.c
 * @brief 网关入口鉴权测试（0.1.15 WS-2 T-24 deny 行为先行 + T-11a/T-17 钉子）。
 *
 * T-24 三条 deny 行为（本文件命名对齐，先写必红、实现后转绿）：
 *   1. t24_post_root_unauthorized_denied —— 未授权 POST / 拒绝；
 *   2. （agent.run PEP 拒绝在 T-11b 落地时补入 cap_registry 测试面）
 *   3. t24_hall_watch_no_credential_denied —— hall/watch 无凭证拒绝。
 *
 * 另钉死：路由敏感性 SSoT 分类、凭证恒定时间比较（T-17）、
 * 回环判定、裁定真值表（fail-closed 语义）。
 */

/* @owner: team-B */
#include "gateway/gateway_auth.h"
#include "gateway/http_gateway_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, desc)                                                      \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("  FAIL: %s (line %d)\n", desc, __LINE__);                  \
        }                                                                      \
    } while (0)

#define TEST_KEY "agentrt-test-key-0115"

static void env_key_set(const char *v)
{
    if (v)
        setenv("GATEWAY_API_KEY", v, 1);
    else
        unsetenv("GATEWAY_API_KEY");
}

/* ---- T-24 前置：路由敏感性分类（SSoT = http_routes 表） ---- */

static void auth_route_sensitive_matrix(void)
{
    /* 敏感面：聚合 JSON-RPC + 全部 SSE + metrics（T-11a 范围） */
    CHECK(http_gateway_route_auth_required("POST", "/") == 1, "POST / is sensitive");
    CHECK(http_gateway_route_auth_required("POST", "/api/v1/chat/stream") == 1,
          "POST chat/stream is sensitive");
    CHECK(http_gateway_route_auth_required("POST", "/api/v1/agent/run/stream") == 1,
          "POST agent/run/stream is sensitive");
    CHECK(http_gateway_route_auth_required("GET", "/api/v1/hall/watch") == 1,
          "GET hall/watch is sensitive");
    CHECK(http_gateway_route_auth_required("GET", "/metrics") == 1, "GET /metrics is sensitive");

    /* 公开面：CORS 预检与健康探针（K8s liveness 不携带凭证） */
    CHECK(http_gateway_route_auth_required("OPTIONS", "/") == 0, "OPTIONS preflight is public");
    CHECK(http_gateway_route_auth_required("GET", "/health") == 0, "GET /health is public");

    /* 未登记路由不在此判定（404/动态端点由 T-11c 监听面收敛处置） */
    CHECK(http_gateway_route_auth_required("GET", "/nope") == 0, "unknown route not classified");
    CHECK(http_gateway_route_auth_required(NULL, "/") == 0, "NULL method safe");
    CHECK(http_gateway_route_auth_required("POST", NULL) == 0, "NULL url safe");
}

/* ---- T-11a：key 配置探测 ---- */

static void auth_key_configured_env(void)
{
    env_key_set(NULL);
    CHECK(gw_auth_key_configured() == 0, "unset key -> not configured");
    env_key_set("");
    CHECK(gw_auth_key_configured() == 0, "empty key -> not configured");
    env_key_set(TEST_KEY);
    CHECK(gw_auth_key_configured() == 1, "set key -> configured");
    env_key_set(NULL);
}

/* ---- T-17：凭证匹配（恒定时间比较语义） ---- */

static void auth_key_matches_cases(void)
{
    env_key_set(TEST_KEY);

    CHECK(gw_auth_key_matches("Bearer " TEST_KEY, NULL) == 1, "valid Bearer accepted");
    CHECK(gw_auth_key_matches(NULL, TEST_KEY) == 1, "valid api_key param accepted");
    CHECK(gw_auth_key_matches("Bearer " TEST_KEY, TEST_KEY) == 1, "both valid accepted");

    CHECK(gw_auth_key_matches(NULL, NULL) == 0, "no credential denied");
    CHECK(gw_auth_key_matches("Bearer wrong-key", NULL) == 0, "wrong Bearer denied");
    CHECK(gw_auth_key_matches("Bearer", NULL) == 0, "bare Bearer scheme denied");
    CHECK(gw_auth_key_matches("Basic " TEST_KEY, NULL) == 0, "non-Bearer scheme denied");
    CHECK(gw_auth_key_matches(NULL, "wrong-key") == 0, "wrong api_key denied");
    /* 前缀/后缀不得通过（异长即拒） */
    CHECK(gw_auth_key_matches("Bearer " TEST_KEY "-extra", NULL) == 0, "longer token denied");
    CHECK(gw_auth_key_matches("Bearer agentrt-test-key-011", NULL) == 0, "shorter token denied");

    env_key_set(NULL);
    /* 未配置时任何凭证都不算匹配（匹配以配置为前提） */
    CHECK(gw_auth_key_matches("Bearer " TEST_KEY, NULL) == 0, "no config -> no match");
}

/* ---- 回环判定（对端 socket 地址 + 绑定 host 共用） ---- */

static void auth_addr_loopback_cases(void)
{
    CHECK(gw_auth_addr_is_loopback("127.0.0.1") == 1, "127.0.0.1 loopback");
    CHECK(gw_auth_addr_is_loopback("127.255.1.2") == 1, "127/8 loopback");
    CHECK(gw_auth_addr_is_loopback("::1") == 1, "::1 loopback");
    CHECK(gw_auth_addr_is_loopback("::ffff:127.0.0.1") == 1, "v4-mapped loopback");

    CHECK(gw_auth_addr_is_loopback("192.168.1.1") == 0, "LAN not loopback");
    CHECK(gw_auth_addr_is_loopback("10.0.0.1") == 0, "private v4 not loopback");
    CHECK(gw_auth_addr_is_loopback("0.0.0.0") == 0, "wildcard host not loopback");
    CHECK(gw_auth_addr_is_loopback("::") == 0, "v6 wildcard not loopback");
    CHECK(gw_auth_addr_is_loopback(NULL) == 0, "NULL not loopback");
    CHECK(gw_auth_addr_is_loopback("") == 0, "empty not loopback");
}

/* ---- T-11a：裁定真值表（fail-closed 语义核心） ---- */

static void auth_decide_matrix(void)
{
    /* 非敏感路由恒放行 */
    CHECK(gw_auth_decide(0, 0, 0, 0) == GW_AUTH_ALLOW, "non-sensitive always allowed");

    /* 已配置凭证：只认有效凭证，回环不豁免 */
    CHECK(gw_auth_decide(1, 1, 1, 0) == GW_AUTH_ALLOW, "sensitive+valid key allowed");
    CHECK(gw_auth_decide(1, 1, 1, 1) == GW_AUTH_ALLOW, "sensitive+valid key+loopback allowed");
    CHECK(gw_auth_decide(1, 0, 1, 1) == GW_AUTH_DENY, "sensitive+configured+invalid denied");
    CHECK(gw_auth_decide(1, 0, 1, 0) == GW_AUTH_DENY, "sensitive+configured+missing denied");

    /* 未配置：仅回环放行（fail-closed），非回环拒绝 */
    CHECK(gw_auth_decide(1, 0, 0, 1) == GW_AUTH_ALLOW, "sensitive+no config+loopback allowed");
    CHECK(gw_auth_decide(1, 0, 0, 0) == GW_AUTH_DENY, "sensitive+no config+remote denied");
}

/* ---- T-24 行为钉子（命名对齐方案 DoD） ---- */

static void t24_post_root_unauthorized_denied(void)
{
    env_key_set(TEST_KEY);
    /* 未授权 POST /：无凭证 → DENY */
    CHECK(gw_auth_decide(http_gateway_route_auth_required("POST", "/"),
                         gw_auth_key_matches(NULL, NULL), gw_auth_key_configured(),
                         gw_auth_addr_is_loopback("203.0.113.5")) == GW_AUTH_DENY,
          "T-24: unauthorized POST / denied");
    env_key_set(NULL);
}

static void t24_hall_watch_no_credential_denied(void)
{
    env_key_set(TEST_KEY);
    /* hall/watch 无凭证：即使来自回环也拒绝（配置凭证后回环不豁免） */
    CHECK(gw_auth_decide(http_gateway_route_auth_required("GET", "/api/v1/hall/watch"),
                         gw_auth_key_matches(NULL, NULL), gw_auth_key_configured(),
                         gw_auth_addr_is_loopback("127.0.0.1")) == GW_AUTH_DENY,
          "T-24: hall/watch without credential denied");
    env_key_set(NULL);
}

int main(void)
{
    printf("[Gateway Auth Tests] WS-2 T-24/T-11a/T-17\n");

    auth_route_sensitive_matrix();
    auth_key_configured_env();
    auth_key_matches_cases();
    auth_addr_loopback_cases();
    auth_decide_matrix();
    t24_post_root_unauthorized_denied();
    t24_hall_watch_no_credential_denied();

    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
