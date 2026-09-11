/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_auth.h
 * @brief HTTP 网关入口鉴权纯策略（0.1.15 WS-2 T-11a/T-17）。
 *
 * 职责边界（K-1）：本模块只做策略判定，不触碰 MHD 连接对象——
 * 请求侧的头部/参数/对端地址提取由 http_gateway_routes.c 完成，
 * 本模块只接收字符串输入，保证全部策略可脱离 libmicrohttpd 单测。
 *
 * fail-closed 语义（WS-2 出口 DoD）：
 *   - GATEWAY_API_KEY 已配置：敏感路由一律要求有效凭证（回环不豁免）；
 *   - 未配置：仅放行回环对端（单机形态开箱可用），非回环一律拒绝；
 *     bind 侧由 gateway_d 强制回环绑定（见 daemons/gateway_d/src/service.c）。
 *
 * 路由敏感性清单（哪些路由需要鉴权）以 http_gateway_routes.c 的
 * http_routes 表 auth_required 字段为单一权威源（SSoT），本模块不复制
 * 路径清单，防两处漂移。
 */

/* @owner: team-B */
#ifndef GATEWAY_AUTH_H
#define GATEWAY_AUTH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 鉴权裁定结果 */
typedef enum {
    GW_AUTH_ALLOW = 0, /* 放行 */
    GW_AUTH_DENY = 1,  /* 拒绝（HTTP 侧映射 401） */
} gw_auth_verdict_t;

/**
 * @brief GATEWAY_API_KEY 是否已配置（非空）。
 * @return 1=已配置；0=未配置
 */
int gw_auth_key_configured(void);

/**
 * @brief 校验请求凭证是否匹配 GATEWAY_API_KEY（T-17：恒定时间比较）。
 *
 * 接受两种形态：Authorization: Bearer <key> 头，或 ?api_key=<key> 参数。
 * 内容比较为恒定时间（长度差异会泄漏长度，属业界可接受范围）。
 *
 * @param authorization  原始 Authorization 头（可为 NULL）
 * @param api_key_param  api_key 查询参数（可为 NULL）
 * @return 1=匹配；0=不匹配或未配置
 */
int gw_auth_key_matches(const char *authorization, const char *api_key_param);

/**
 * @brief 判断地址字符串是否为回环地址。
 *
 * 同时服务于两个场景：对端 socket 地址（鉴权判定）与绑定 host
 * （bind 侧收敛）。识别 127.0.0.0/8、::1、IPv4 映射形式 ::ffff:127.x。
 *
 * @param addr 数值形式 IP 字符串（inet_ntop 产物；可为 NULL）
 * @return 1=回环；0=非回环或空
 */
int gw_auth_addr_is_loopback(const char *addr);

/**
 * @brief 入口鉴权裁定（纯策略，WS-2 T-11a 核心）。
 *
 * @param route_sensitive 该路由是否敏感（来自路由表 auth_required）
 * @param key_valid      凭证是否匹配（gw_auth_key_matches 结果）
 * @param key_configured 是否配置了 GATEWAY_API_KEY
 * @param peer_loopback  对端是否回环地址
 * @return GW_AUTH_ALLOW / GW_AUTH_DENY
 */
gw_auth_verdict_t gw_auth_decide(int route_sensitive, int key_valid, int key_configured,
                                 int peer_loopback);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_AUTH_H */
