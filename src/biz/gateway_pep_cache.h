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

/* M2-S4（0.1.9 §3.3.1）：订阅 notify_d topic=airy.cupolas.epoch 主动失效
 * 缓存（SSE 长连接 + 自动重连）。进程级单次启动；fail-open——notify_d
 * 不可达仅告警重试，不影响懒对齐路径（miss RPC 携带权威 epoch 兜底）。 */
void gw_pep_epoch_observe(const gateway_business_ctx_t *ctx);

/* 从 notify SSE data 帧解析权威 epoch；非 airy.cupolas.epoch 帧返回 0。
 * 公开供单元测试验证帧解析（消息内层 JSON 未转义，按键值容错解析）。 */
uint64_t gw_pep_epoch_parse(const char *data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_PEP_CACHE_H */
