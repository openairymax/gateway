/* SPDX-FileCopyrightText: 2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_policy.h
 * @brief 统一能力网关 — 命名空间策略面（0.1.19 机制与策略分离）。
 *
 * 命名空间的归属、路由、超时、wire 前缀与 L2 透传语义统一由
 * gateway/.manifest 声明，构建期经 tools/codegen/gateway_gen.py 生成
 * 只读策略表。本头文件是策略表在 C 侧的唯一读取契约：路由、限频、
 * 方法名转换均经此查询，禁止在表外再维护一份命名空间清单。
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_POLICY_H
#define AIRY_RT_DAEMON_GATEWAY_POLICY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 策略表容量上限（生成期静态断言，运行期零分配）。 */
#define GW_NS_POL_MAX 32

/* 命名空间策略项（字段顺序与生成表一致）。 */
typedef struct {
    const char *ns;         /* 外部命名空间前缀 */
    const char *owner;      /* 独占 daemon 短名（gateway = 网关内实现域） */
    const char *daemon;     /* 路由宿主进程名（非路由为 NULL） */
    const char *sock;       /* ctx 端点绑定键（非路由为 NULL） */
    unsigned int timeout_ms;
    const char *prefix;     /* wire 方法前缀（无前缀为 NULL） */
    int l2_pass;            /* L2 三件套透传宿主 */
    int route;              /* 可经 syscall 派发 */
} gw_ns_pol_t;

/**
 * @brief 按命名空间查策略项（容忍尾点，如 "llm." 与 "llm" 等价）。
 * @return 策略项指针（静态存储）；未知返回 NULL
 */
const gw_ns_pol_t *gw_ns_pol(const char *ns);

/**
 * @brief 策略项在生成表中的槽位（按需激活限频数组下标）。
 * @return 0..N-1；非表内指针返回 -1
 */
int gw_ns_slot(const gw_ns_pol_t *pol);

/**
 * @brief 判断方法是否属于 L2 标准三件套（shutdown/get_stats/health_check）。
 * @return 1=是；0=否
 */
int gw_l2_method(const char *method);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_POLICY_H */
