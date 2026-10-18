/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_cap_registry.c
 * @brief 统一能力网关 — 能力注册表与命名空间策略查询面。
 *
 * 0.1.19 起，能力注册表、命名空间归属、超时、wire 前缀与 L2 透传语义
 * 统一由 gateway/.manifest 声明，构建期经 tools/codegen/gateway_gen.py
 * 生成只读表（gateway_policy_gen.inc）。本文件不含策略数据，仅提供
 * 查询与不变量校验：表外能力一律 fail-closed 拒绝。
 */

#include "gateway_cap_registry.h"

#include "gateway_policy.h"

#include "gateway_biz_internal.h"

#include "logging.h"
#include "airy_memory.h"

#include "gateway_hall_store.h"

#include <stddef.h>
#include <string.h>

#include "gateway_policy_gen.inc"

_Static_assert(GW_NS_POLICY_N <= GW_NS_POL_MAX,
               "gateway namespace policy exceeds GW_NS_POL_MAX");

const gw_ns_pol_t *gw_ns_pol(const char *ns)
{
    char buf[32];
    size_t n;

    if (!ns)
        return NULL;
    n = strlen(ns);
    if (n == 0 || n >= sizeof(buf))
        return NULL;
    AIRY_MEMCPY(buf, ns, n);
    buf[n] = '\0';
    if (buf[n - 1] == '.') {
        buf[--n] = '\0';
        if (n == 0)
            return NULL;
    }
    for (size_t i = 0; i < GW_NS_POLICY_N; ++i) {
        if (strcmp(buf, GW_NS_POLICY[i].ns) == 0)
            return &GW_NS_POLICY[i];
    }
    return NULL;
}

int gw_ns_slot(const gw_ns_pol_t *pol)
{
    ptrdiff_t idx;

    if (!pol)
        return -1;
    idx = pol - GW_NS_POLICY;
    if (idx < 0 || (size_t)idx >= GW_NS_POLICY_N)
        return -1;
    return (int)idx;
}

int gw_l2_method(const char *method)
{
    if (!method)
        return 0;
    for (size_t i = 0; i < GW_L2_METHODS_N; ++i) {
        if (strcmp(method, GW_L2_METHODS[i]) == 0)
            return 1;
    }
    return 0;
}

const gw_cap_t *gw_cap_find(const char *cap_key)
{
    if (!cap_key)
        return NULL;
    for (size_t i = 0; i < GW_CAP_REGISTRY_N; ++i) {
        if (strcmp(GW_CAP_REGISTRY[i].cap_key, cap_key) == 0)
            return &GW_CAP_REGISTRY[i];
    }
    return NULL;
}

int gw_cap_ns_timeout(const char *ns)
{
    const gw_ns_pol_t *pol = gw_ns_pol(ns);

    return pol ? (int)pol->timeout_ms : GW_TOOL_TIMEOUT_MS;
}

int gw_cap_version(const char *cap_key)
{
    const gw_cap_t *cap = gw_cap_find(cap_key);

    return cap ? cap->version : -1;
}

int gw_cap_check_version(const char *cap_key, int req_version)
{
    const gw_cap_t *cap = gw_cap_find(cap_key);

    if (!cap)
        return -1;
    if (req_version <= 0)
        return 0; /* 调用方未声明版本：仅存在性校验 */
    return (req_version == cap->version) ? 0 : 1;
}

const char *gw_cap_perm_for(const char *cap_key)
{
    const gw_cap_t *cap = gw_cap_find(cap_key);

    return cap ? cap->perm : NULL; /* 未声明权限 = 默认放行 */
}

/* namespace 独占性门禁（边界契约执行机制）：
 *   - 每个注册 cap_key 的命名空间前缀必须已登记归属（未登记 = 新
 *     namespace 未声明独占，fail-closed）
 *   - FWD 转发目标必须与归属 daemon 一致（namespace 归 A 独占却转发
 *     到 B，即边界漂移，fail-closed）
 * 生成期已完成同一校验；启动期保留本断言作纵深防御。
 * @return 0=通过；非 0=冲突（gateway 启动期据此拒启） */
int gw_cap_ns_validate(void)
{
    size_t i;

    for (i = 0; i < GW_CAP_REGISTRY_N; ++i) {
        const gw_cap_t *cap = &GW_CAP_REGISTRY[i];
        const char *dot = strchr(cap->cap_key, '.');
        size_t prefix_len;
        const char *owner = NULL;
        size_t j;

        if (!dot) {
            AIRY_LOG_ERROR("cap registry: invalid cap_key '%s' (no '.')",
                           cap->cap_key);
            return -1;
        }
        prefix_len = (size_t)(dot - cap->cap_key);
        for (j = 0; j < GW_NS_POLICY_N; ++j) {
            if (strlen(GW_NS_POLICY[j].ns) == prefix_len &&
                strncmp(GW_NS_POLICY[j].ns, cap->cap_key, prefix_len) == 0) {
                owner = GW_NS_POLICY[j].owner;
                break;
            }
        }
        if (!owner) {
            AIRY_LOG_ERROR("cap registry: namespace '%.*s' (cap_key '%s') has "
                           "no registered owner daemon",
                           (int)prefix_len, cap->cap_key, cap->cap_key);
            return -1;
        }
        if (cap->kind == GW_CAP_KIND_FWD && strcmp(cap->ns, owner) != 0) {
            AIRY_LOG_ERROR("cap registry: cap_key '%s' forwards to '%s' but "
                           "namespace '%.*s' is exclusively owned by '%s'",
                           cap->cap_key, cap->ns, (int)prefix_len,
                           cap->cap_key, owner);
            return -1;
        }
    }
    return 0;
}

void gw_cap_emit(const char *cap_key, const char *status, const char *detail)
{
    if (!cap_key || !status)
        return;
    cJSON *content = cJSON_CreateObject();
    if (!content)
        return;
    cJSON_AddStringToObject(content, "cap_key", cap_key);
    cJSON_AddStringToObject(content, "status", status);
    if (detail && *detail)
        cJSON_AddStringToObject(content, "detail", detail);
    char *content_str = cJSON_PrintUnformatted(content);
    cJSON_Delete(content);
    if (!content_str)
        return;
    /* 事件流单一真相源：cap 类别事件（task_id 用 "gateway" 汇聚网关侧能力调用） */
    (void)gw_hall_store_event("gateway", "cap", NULL, content_str);
    AIRY_FREE(content_str);
}
