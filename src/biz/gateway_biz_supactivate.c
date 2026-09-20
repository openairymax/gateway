/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
/**
 * @file gateway_biz_supactivate.c
 * @brief supervisor.activate 单向通知实现（0.1.18 §12.13 B13 / V13.2）。
 *
 * 端点解析与 supervisor_d 侧 sup_decl_load 保持同构：
 *   AIRY_SUPERVISOR_SOCK 覆盖 -> WIN32 127.0.0.1:8095 ->
 *   airy_runtime_dir()/supervisor.sock。
 * 传输复用南向唯一客户端面（gate N2）：gw_svc_call -> gw_aipc_call。
 */

#include "gateway_biz_supactivate.h"

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW_SUP_ACT_METHOD "supervisor.activate"
/* 控制口串行处理、连接处理完即回，短超时即够（fire-and-forget 不重试）。 */
#define GW_SUP_ACT_TIMEOUT_MS 500

static void sup_ctrl_ep(char *out, size_t out_sz)
{
    const char *env = getenv("AIRY_SUPERVISOR_SOCK");
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_sz);
        return;
    }
#ifdef _WIN32
    AIRY_STRNCPY_TERM(out, "127.0.0.1:8095", out_sz);
#else
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir)
        snprintf(out, out_sz, "%s/supervisor.sock", run_dir);
    else
        AIRY_STRNCPY_TERM(out, "supervisor.sock", out_sz);
#endif
}

void gw_sup_notify_activate(const char *daemon)
{
    if (!daemon || !*daemon)
        return;

    char ep[256];
    sup_ctrl_ep(ep, sizeof(ep));

    char params[64];
    snprintf(params, sizeof(params), "{\"name\":\"%s\"}", daemon);

    char *resp = gw_svc_call(ep, GW_SUP_ACT_METHOD, params, GW_SUP_ACT_TIMEOUT_MS);
    if (!resp) {
        AIRY_LOG_WARN("gateway sup_activate: supervisor ctrl unreachable daemon=%s", daemon);
        return;
    }
    if (strstr(resp, "\"error\""))
        AIRY_LOG_INFO("gateway sup_activate: supervisor refused daemon=%s resp=%s", daemon, resp);
    else
        AIRY_LOG_INFO("gateway sup_activate: accepted daemon=%s", daemon);
    AIRY_FREE(resp);
}
