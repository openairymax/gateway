// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_agent.c
 * @brief Gateway agent.run / agent.cancel 转发域（M1-1a 引擎下沉）。
 *
 * agent.run 进程内引擎（会话注册表 / GCCP 双思考 / 编排 / ReAct 工具循环 /
 * mem 持久化 / hall 事件）已迁入 agent_d（agent_run_engine.c +
 * agent_run_loop.c），gateway 本文件仅做协议转发：
 *   - agent.run        -> agent_d "run"（参数原样透传，响应 id 重写）
 *   - agent.cancel     -> agent_d "run_cancel"（session_id 透传）
 * gateway 不再承载任何 agent.run 业务分支（K-1 纯翻译）。
 */

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"
#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* agent.run 长耗时（LLM 往返 + 工具循环），转发超时取最大档 */
#define GW_RUN_FWD_TIMEOUT_MS 600000

/**
 * @brief agent.run 转发：解析最小入参契约（prompt 存在性校验），其余
 *        参数原样透传 agent_d "run"，响应 id 重写为请求 id。
 */
char *handle_agent_run(cJSON *root, gateway_business_ctx_t *ctx)
{
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    const char *prompt = NULL;
    if (params) {
        cJSON *p = cJSON_GetObjectItem(params, "prompt");
        if (cJSON_IsString(p)) {
            prompt = p->valuestring;
        } else {
            cJSON *messages = cJSON_GetObjectItem(params, "messages");
            cJSON *m0 = (messages && cJSON_GetArraySize(messages) > 0) ?
                            cJSON_GetArrayItem(messages, 0) :
                            NULL;
            cJSON *c = m0 ? cJSON_GetObjectItem(m0, "content") : NULL;
            if (cJSON_IsString(c))
                prompt = c->valuestring;
        }
    }
    if (!prompt || !*prompt) {
        return jsonrpc_error(-32602, "Invalid params: missing prompt", id);
    }

    char *params_str = params ? cJSON_PrintUnformatted(params) : AIRY_STRDUP("{}");
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    /* 架构约束 2026-08-25 "必须走 syscall": agent.run 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("agent", "run", params_str, GW_RUN_FWD_TIMEOUT_MS, &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        return jsonrpc_error(-32603, "Agent service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Agent service returned invalid response", id);
    }

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *svc_id = cJSON_GetObjectItem(rroot, "id");
    if (svc_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/**
 * @brief agent.cancel 转发：params.session_id 原样透传 agent_d "run_cancel"，
 *        响应 id 重写为请求 id。
 */
char *handle_agent_cancel(cJSON *root, gateway_business_ctx_t *ctx)
{
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *sid = params ? cJSON_GetObjectItem(params, "session_id") : NULL;
    if (!cJSON_IsString(sid) || !sid->valuestring || !*sid->valuestring) {
        return jsonrpc_error(-32602, "Invalid params: missing session_id", id);
    }

    char *params_str = params ? cJSON_PrintUnformatted(params) : AIRY_STRDUP("{}");
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    /* 架构约束 2026-08-25 "必须走 syscall": agent.run_cancel 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("agent", "run_cancel", params_str, GW_TOOL_TIMEOUT_MS,
                                      &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        return jsonrpc_error(-32603, "Agent service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Agent service returned invalid response", id);
    }

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *svc_id = cJSON_GetObjectItem(rroot, "id");
    if (svc_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}
