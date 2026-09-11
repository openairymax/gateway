// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_auth.c
 * @brief 网关入口鉴权纯策略实现（0.1.15 WS-2 T-11a/T-17）。
 *
 * fail-closed 双层语义（与 gateway_auth.h 约定一致）：
 *   1. 请求侧：GATEWAY_API_KEY 已配置 → 敏感路由强制凭证（回环对端不豁免）；
 *      未配置 → 仅放行回环对端，非回环一律拒绝。
 *   2. bind 侧（service.c）：无凭证时非回环监听地址强制收敛 127.0.0.1，
 *      保证"绑定地址与日志同源"（日志 WARN 告知实际监听面）。
 *
 * 本模块为纯策略（K-1）：不触碰 MHD、不做 IO，输入全部为已提取的标量，
 * 可独立单元测试（tests/test_gateway_auth.c）。
 */

/* @owner: team-B */
#include "gateway/gateway_auth.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 恒定时间字节比较（T-17）
 *
 * diff 经 volatile 累积，阻止编译器短路优化导致的时序侧信道。
 * 长度不等直接失败——长度本身可探测属业界可接受（与主流 HTTP 框架一致）。
 */
static int auth_ct_memeq(const char *a, const char *b, size_t len)
{
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0 ? 1 : 0;
}

int gw_auth_key_configured(void)
{
    const char *key = getenv("GATEWAY_API_KEY");
    return (key && key[0]) ? 1 : 0;
}

int gw_auth_key_matches(const char *authorization, const char *api_key_param)
{
    const char *env_key = getenv("GATEWAY_API_KEY");
    if (!env_key || !env_key[0])
        return 0; /* 未配置：任何凭证均不匹配（fail-closed，防误配为公开面） */

    const char *token = NULL;
    if (authorization && strncmp(authorization, "Bearer ", 7) == 0 && authorization[7]) {
        token = authorization + 7;
    } else if (api_key_param && api_key_param[0]) {
        token = api_key_param;
    }
    if (!token)
        return 0;

    size_t token_len = strlen(token);
    if (token_len != strlen(env_key))
        return 0;

    return auth_ct_memeq(token, env_key, token_len);
}

int gw_auth_addr_is_loopback(const char *addr)
{
    if (!addr || !addr[0])
        return 0;
    /* IPv4 回环 127.0.0.0/8（整个 /8 均为回环，RFC 1122） */
    if (strncmp(addr, "127.", 4) == 0)
        return 1;
    /* IPv6 本机 ::1 */
    if (strcmp(addr, "::1") == 0)
        return 1;
    /* IPv4-mapped IPv6 回环 ::ffff:127.x（双栈 socket 对端可能以此形态呈现） */
    if (strncmp(addr, "::ffff:127.", 11) == 0)
        return 1;
    return 0;
}

gw_auth_verdict_t gw_auth_decide(int route_sensitive, int key_valid, int key_configured,
                                 int peer_loopback)
{
    if (!route_sensitive)
        return GW_AUTH_ALLOW; /* 公开面白名单：OPTIONS 预检、/health（K8s 探针） */

    if (key_configured)
        return key_valid ? GW_AUTH_ALLOW : GW_AUTH_DENY; /* 有凭证要求即强制，回环不豁免 */

    return peer_loopback ? GW_AUTH_ALLOW : GW_AUTH_DENY; /* 无配置：仅回环放行 */
}
