// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gateway_biz_tools.c
 * @brief Gateway MCP 工具目录注册域（M1-1a 工具目录 SSoT 收敛）。
 *
 * 内置工具目录的唯一权威在 tool_d（registry + service_builtin.c 注册，
 * list_tools 输出 input_schema）；gateway 不再本地硬编码任何工具 schema，
 * 启动时经 tool.list_tools 拉取目录并注册到 MCP server（exec_fn 仍为
 * gw_biz_tool_exec，工具执行经 tool_d.execute_tool）。工具新增/参数变更
 * 只需改 tool_d，gateway 零改动。
 */

#include "gateway_mcp_server.h"
#include "gateway_business_handler.h"
#include "gateway_biz_internal.h"

#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 从 tool_d 拉取内置工具目录并注册到 gateway MCP server。
 * @return 注册失败数（0 = 全部成功）；tool_d 不可达时返回 -1（调用方告警）。
 */
int gw_biz_mcp_register_tools(gw_mcp_server_t *mcp, void *user_data)
{
    if (!mcp)
        return -1;

    /* 架构约束 2026-08-25 "必须走 syscall": tool.list_tools 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("tool", "list_tools", "{}", GW_TOOL_TIMEOUT_MS, &resp);
    if (rc != AIRY_SUCCESS || !resp) {
        AIRY_LOG_WARN("gateway: tool.list_tools unreachable, MCP tool catalog empty");
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        AIRY_LOG_WARN("gateway: tool.list_tools returned invalid response");
        return -1;
    }
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        AIRY_LOG_WARN("gateway: tool.list_tools returned error");
        cJSON_Delete(root);
        return -1;
    }

    int failed = 0;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *tools = result ? cJSON_GetObjectItem(result, "tools") : NULL;
    if (!cJSON_IsArray(tools) || cJSON_GetArraySize(tools) == 0) {
        /* 兼容 tool_d 直接把数组放在 result 的旧格式 */
        tools = result;
    }
    if (cJSON_IsArray(tools)) {
        int n = cJSON_GetArraySize(tools);
        for (int i = 0; i < n; i++) {
            cJSON *t = cJSON_GetArrayItem(tools, i);
            cJSON *name = cJSON_GetObjectItem(t, "name");
            cJSON *desc = cJSON_GetObjectItem(t, "description");
            cJSON *schema = cJSON_GetObjectItem(t, "input_schema");
            if (!cJSON_IsString(name) || !name->valuestring || !name->valuestring[0]) {
                failed++;
                continue;
            }
            char *schema_str = cJSON_IsObject(schema) ? cJSON_PrintUnformatted(schema) :
                                                        AIRY_STRDUP("{}");
            if (!schema_str) {
                failed++;
                continue;
            }
            int rrc = gw_mcp_server_register_tool(
                mcp, name->valuestring,
                cJSON_IsString(desc) && desc->valuestring ? desc->valuestring : "",
                schema_str, gw_biz_tool_exec, user_data);
            AIRY_FREE(schema_str);
            if (rrc != 0) {
                AIRY_LOG_WARN("gateway: failed to register tool '%s' (rc=%d)",
                         name->valuestring, rrc);
                failed++;
            }
        }
    }
    cJSON_Delete(root);
    return failed;
}
