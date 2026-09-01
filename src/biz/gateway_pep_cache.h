/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_pep_cache.h
 * @brief Gateway PEP（策略执行点）裁定缓存（M2-S5，0.1.9 §3.2）。
 *
 * gateway 工具调用热路径的权限裁定缓存：
 *   - 命中（三元组 key + epoch 一致）直接返回，零 RPC 延迟
 *   - miss 时向 PDP（cupolas_d check_permission）请求裁定，响应携带
 *     权威 epoch；epoch 变化即整体失效（策略版本 SSoT）
 *   - RPC 不可达降级本地 ACL（daemon_check_tool_permission，fail-closed）
 */

#ifndef AIRY_RT_GATEWAY_PEP_CACHE_H
#define AIRY_RT_GATEWAY_PEP_CACHE_H

#include <stdint.h>

#include "gateway_biz_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

void gw_pep_init(void);
void gw_pep_clear(void);

/* 返回 0 = allow，非 0 = deny（fail-closed） */
int gw_pep_check(const gateway_business_ctx_t *ctx, const char *agent, const char *tool,
                 const char *action);

/* 当前已对齐的权威 epoch（0 = 尚未对齐） */
uint64_t gw_pep_epoch(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_PEP_CACHE_H */
