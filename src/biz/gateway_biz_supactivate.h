/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
/**
 * @file gateway_biz_supactivate.h
 * @brief 按需激活单向通知（0.1.18 §12.13 B13 / V13.2）。
 *
 * gateway 南向转发不可达（daemon 未在运行）时，向 supervisor_d 控制口单向
 * 发送 supervisor.activate：aux daemon 在首次总线调用后出现。本接口只负责
 * "通知到达"，不改变调用方的错误语义——客户端可见结果仍为 unreachable，
 * 重试责任在客户端（项目禁盲重试哲学）。
 */

#ifndef AIRY_RT_GATEWAY_BIZ_SUPACTIVATE_H
#define AIRY_RT_GATEWAY_BIZ_SUPACTIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单向请求 supervisor 激活指定 daemon（fire-and-forget）。
 * @param daemon supervisor 登记名（如 "monit_d"；<x>_d 与 sock <x>.sock 同源）
 *
 * supervisor 侧幂等（pid 有效直接返回）且不绕过退避；传输失败或 supervisor
 * 拒绝（未登记/spawn 失败）仅记日志，不向调用方报错。限频由调用方持有。
 */
void gw_sup_notify_activate(const char *daemon);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_BIZ_SUPACTIVATE_H */
