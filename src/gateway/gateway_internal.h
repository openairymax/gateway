/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file gateway_internal.h
 * @brief Gateway 内部实现头：仅承载内部专属定义。
 *
 * 公共 API（类型、回调签名、全部函数声明）的唯一权威是
 * include/gateway.h；本头文件通过包含它获得公共契约，自身只补充
 * 翻译单元内部使用的：GATEWAY_VERSION 回退、internal handler 签名、
 * 处理回调能力格、ops 分派表与 struct gateway 完整布局（公共头保持
 * opaque）。
 */

/* @owner: team-B */
#ifndef AIRY_RT_GATEWAY_INTERNAL_H
#define AIRY_RT_GATEWAY_INTERNAL_H

#include "../include/gateway.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* 版本 SSoT：网关对外报告版本与 agentrt 全系统版本一致，唯一权威为
 * VERSION 文件 → 顶层 CMakeLists.txt 注入的 AIRYRT_VERSION。构建期
 * gateway/CMakeLists.txt 与 daemons/gateway_d/CMakeLists.txt 亦从 VERSION
 * 注入 GATEWAY_VERSION。此处按优先级取 AIRYRT_VERSION（C 侧 SSoT）；若
 * 两者均未定义（未走 CMake 的独立语法检查），回退 "0.0.0-dev" 明确标识
 * "非发布构建"，不在源码内保留任何真实发布号副本（漂移免疫）。 */
#ifndef GATEWAY_VERSION
#ifdef AIRYRT_VERSION
#define GATEWAY_VERSION AIRYRT_VERSION
#else
#define GATEWAY_VERSION "0.0.0-dev"
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef char *(*gateway_internal_handler_t)(void *request, void *user_data);

/* 能力格：请求处理回调的绑定槽。HTTP/HTTP2/WS/Stdio 四类后端共用同一绑定
 * 语义——绑定机制唯一收敛于 gateway_api.c 的 gw_handler_bind()，后端只持有
 * 槽位，不再各自维护 handler/handler_data 散字段与适配器所有权。 */
typedef struct gateway_handler_slot {
    gateway_internal_handler_t handler;
    void *data;
} gateway_handler_slot_t;

/* 绑定/清空槽位；handler 为 NULL 即解绑，data 一并清零。 */
void gw_handler_bind(gateway_handler_slot_t *slot, gateway_internal_handler_t handler,
                     void *user_data);

typedef struct gateway_ops {
    int (*start)(void *impl);
    void (*stop)(void *impl);
    void (*destroy)(void *impl);
    const char *(*get_name)(void *impl);
    int (*get_stats)(void *impl, char **out_json);
    bool (*is_running)(void *impl);
    int (*set_handler)(void *impl, gateway_internal_handler_t handler, void *user_data);
} gateway_ops_t;

/* 完整布局仅内部翻译单元可见；gateway_t 名字由公共头 opaque typedef 提供。 */
struct gateway {
    const gateway_ops_t *ops;
    void *impl;
    gateway_type_t type;
    gateway_request_handler_t public_handler;
    void *public_handler_data;
};

#ifdef __cplusplus
}
#endif

#endif
