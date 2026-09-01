/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_cap_registry.h
 * @brief 统一能力网关 — 能力注册表（cap_key 单一权威源，0.1.6 P1-4）。
 *
 * gateway 对外暴露的全部能力（<ns>.<method>）在此注册表单一声明，是
 * SSoT（Unify Design）：外部可调用能力的枚举、路由目标、处理方式、超时
 * 均以此表为准。命名空间转发白名单、特殊处理器（mem/llm.list_models/
 * tool.pending|approve/hall/agent.run|cancel）从本表派生，禁止在表外
 * 再维护一份方法清单（防漂移）。
 *
 * cap_key 语义 = "<ns>.<method>"（如 "llm.complete"）。未登记的
 * cap_key 一律 fail-closed 拒绝（-32601），保证"只暴露已声明能力"。
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_CAP_REGISTRY_H
#define AIRY_RT_DAEMON_GATEWAY_CAP_REGISTRY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 能力处理方式 */
typedef enum {
    GW_CAP_KIND_FWD = 0,       /* 命名空间转发：airy_sys_svc_call(ns, method) */
    GW_CAP_KIND_MEM,           /* mem.* 特殊处理（AIRY_GATEWAY_MEM_PUBLIC 门控） */
    GW_CAP_KIND_LLM_LIST,      /* llm.list_models 特殊处理（附 default 信息） */
    GW_CAP_KIND_TOOL_APPROVE,  /* tool.pending / tool.approve 审批特殊处理 */
    GW_CAP_KIND_HALL,          /* hall.* 网关内实现（任务看板/事件流/决策链） */
    GW_CAP_KIND_AGENT_RUN,     /* agent.run / agent.cancel 网关内编排 */
} gw_cap_kind_t;

/* 能力注册项 */
typedef struct {
    const char *cap_key;  /* 单一权威键："<ns>.<method>" */
    const char *ns;       /* 目标 daemon 命名空间 */
    const char *method;   /* L2 方法名（转发/处理的目标方法） */
    gw_cap_kind_t kind;   /* 处理方式 */
    int version;          /* 能力契约版本（0 = 默认契约版本 1；见 gw_cap_version） */
} gw_cap_t;

/**
 * @brief 按 cap_key 查找能力注册项（SSoT 存在性校验核心）。
 * @param cap_key 如 "llm.complete"
 * @return 注册项指针；未登记返回 NULL（调用方 fail-closed 拒绝）
 */
const gw_cap_t *gw_cap_find(const char *cap_key);

/**
 * @brief 返回注册能力总数（供自检/门禁断言，防误删）。
 */
size_t gw_cap_count(void);

/**
 * @brief 按索引取注册项（供枚举/自检；index < gw_cap_count()）。
 * @param index 0-based 索引
 * @return 注册项指针；越界返回 NULL
 */
const gw_cap_t *gw_cap_at(size_t index);

/**
 * @brief 按命名空间取转发超时（ms；未知命名空间返回默认 90s）。
 */
int gw_cap_ns_timeout(const char *ns);

/**
 * @brief 能力契约版本（0.1.6 P1-4 SSoT 契约版本校验维度）。
 * @param cap_key 能力键
 * @return 契约版本号；未登记能力返回 -1。未显式标注版本的能力
 *         视为默认契约版本 1（GW_CAP_VERSION_DEFAULT）。
 */
int gw_cap_version(const char *cap_key);

/**
 * @brief 契约版本比对（SSoT 校验）：调用方声明版本 vs 注册表版本。
 * @param cap_key     能力键
 * @param req_version 调用方声明版本（<=0 视为未声明，仅做存在性检查）
 * @return 0=通过（版本匹配或未声明）；1=版本不匹配；-1=能力未登记
 */
int gw_cap_check_version(const char *cap_key, int req_version);

/**
 * @brief 能力所需权限（0.1.6 P1-4 SSoT 权限校验维度）。
 *
 * 仅高敏能力声明所需权限（GW_CAP_EXTRA_PERM）；未声明（返回 NULL）=
 * 无需额外权限，默认放行（llm/think/agent/tool/mem 等核心链路）。
 * 已声明权限须经 daemon_security ACL 检查（fail-closed：未注册规则拒绝，
 * 管理员在 permission_rules.yaml 显式授权）。
 *
 * @param cap_key 能力键
 * @return 所需权限名；NULL=无需额外权限。未登记能力返回 NULL。
 */
const char *gw_cap_perm_for(const char *cap_key);

/**
 * @brief 能力调用事件埋点（事件流单一真相源）：网关每受理一个能力调用
 *        记录一条 "cap" 类别事件，含 cap_key 与结果状态。
 * @param cap_key 能力键
 * @param status  "ok" / "deny"（未登记/白名单外） / "fail"（服务不可达）
 * @param detail  可选附加 JSON 内容（NULL 可）
 */
void gw_cap_emit(const char *cap_key, const char *status, const char *detail);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_CAP_REGISTRY_H */
