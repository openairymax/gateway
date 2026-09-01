// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_backend.c
 * @brief Gateway protocol backends: business-execution interfaces of the
 *        MCP / OpenAI / A2A adapters.
 *
 * Translates calls of the three external protocols (MCP tools/call, OpenAI
 * chat/completions, A2A task) into internal daemon service calls
 * (tool_d.execute_tool / llm_d.complete / sched_d.schedule_task), for use
 * by the adapters registered with gateway_protocol_router.
 *
 * Split from gateway_business_handler.c (single responsibility: external
 * protocol backends).
 */

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"

#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief MCP tool execution backend: tools/call -> tool_d.execute_tool
 *
 * The returned result_json is a valid JSON string (quoted, so MCP tools/call's
 * content[].text can embed it directly via %s); its content is the tool_d
 * execution output or an error description.
 */
int gw_biz_tool_exec(const char *tool_name, const char *arguments_json, char **result_json,
                     void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *result_json = NULL;
    if (!ctx || !tool_name) {
        *result_json = AIRY_STRDUP("\"Invalid tool request\"");
        return -1;
    }

    if (gw_acl_check_tool(tool_name) != 0) {
        *result_json = AIRY_STRDUP("\"Permission denied: tool not authorized\"");
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params) {
        *result_json = AIRY_STRDUP("\"Out of memory\"");
        return -1;
    }
    cJSON_AddStringToObject(params, "tool_id", tool_name);
    cJSON *pargs = cJSON_Parse(arguments_json && arguments_json[0] ? arguments_json : "{}");
    cJSON_AddItemToObject(params, "params", pargs ? pargs : cJSON_CreateObject());
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str) {
        *result_json = AIRY_STRDUP("\"Out of memory\"");
        return -1;
    }

    /* 架构约束 2026-08-25 "必须走 syscall": tool.execute_tool 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("tool", "execute_tool", params_str, GW_TOOL_TIMEOUT_MS,
                                      &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        *result_json = AIRY_STRDUP("\"Tool service unreachable\"");
        return -1;
    }

    char *text = NULL;
    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *result_json = AIRY_STRDUP("\"Tool service returned invalid response\"");
        return -1;
    }
    cJSON *err = cJSON_GetObjectItem(root, "error");
    cJSON *result = err ? NULL : cJSON_GetObjectItem(root, "result");
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        if (cJSON_IsNumber(success) && success->valueint != 0 && cJSON_IsString(output)) {
            text = AIRY_STRDUP(output->valuestring);
        } else if (cJSON_IsString(error) && error->valuestring) {
            size_t n = strlen(error->valuestring) + 16;
            text = (char *)AIRY_MALLOC(n);
            if (text)
                snprintf(text, n, "Error: %s", error->valuestring);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            size_t n = strlen(msg->valuestring) + 16;
            text = (char *)AIRY_MALLOC(n);
            if (text)
                snprintf(text, n, "Error: %s", msg->valuestring);
        }
    }
    cJSON_Delete(root);
    if (!text)
        text = AIRY_STRDUP("(no output)");

    cJSON *jstr = cJSON_CreateString(text);
    AIRY_FREE(text);
    *result_json = jstr ? cJSON_PrintUnformatted(jstr) : AIRY_STRDUP("\"\"");
    if (jstr)
        cJSON_Delete(jstr);
    return 0;
}

/**
 * @brief OpenAI embeddings backend: /v1/embeddings -> llm_d.embeddings
 *
 * Forwards the OpenAI-format embeddings request to llm_d, which proxies it to
 * the owning provider's $api_base/embeddings; the upstream OpenAI-format JSON
 * (data[].embedding) is returned verbatim so clients never see the internal
 * JSON-RPC.
 */
int gw_biz_llm_embeddings(const char *model, const char *input_json, char **response_json,
                          void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *response_json = NULL;
    if (!ctx)
        return -1;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "model", (model && model[0]) ? model : ctx->default_model);
    cJSON *input = cJSON_Parse(input_json && input_json[0] ? input_json : "[]");
    cJSON_AddItemToObject(params, "input", input ? input : cJSON_CreateArray());
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    /* 架构约束 2026-08-25 "必须走 syscall": llm.embeddings 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("llm", "embeddings", params_str, GW_LLM_DEFAULT_TIMEOUT_MS,
                                      &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp)
        return -1;

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -1;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        *response_json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return 0;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    *response_json = cJSON_PrintUnformatted(result ? result : root);
    cJSON_Delete(root);
    if (!*response_json)
        return -1;
    return 0;
}

/**
 * @brief OpenAI LLM backend: chat/completions -> llm_d.complete
 *
 * Calls llm_d and converts the response into the OpenAI chat.completion format
 * (choices[0].message.content / tool_calls) so clients never see the internal
 * JSON-RPC.
 */
int gw_biz_llm_complete(const char *model, const char *messages_json, const char *functions_json,
                        double temperature, int max_tokens, char **response_json, void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *response_json = NULL;
    if (!ctx) {
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "model", (model && model[0]) ? model : ctx->default_model);
    cJSON *msgs = cJSON_Parse(messages_json && messages_json[0] ? messages_json : "[]");
    cJSON_AddItemToObject(params, "messages", msgs ? msgs : cJSON_CreateArray());

    if (functions_json && functions_json[0]) {
        cJSON *tools = cJSON_Parse(functions_json);
        if (tools) {
            cJSON_AddItemToObject(params, "tools", tools);
        } else {
            cJSON_AddItemToObject(params, "tools", cJSON_CreateArray());
        }
    }
    cJSON_AddNumberToObject(params, "max_tokens", max_tokens > 0 ? max_tokens : 2048);
    cJSON_AddNumberToObject(params, "temperature", temperature);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    /* 架构约束 2026-08-25 "必须走 syscall": llm.complete 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("llm", "complete", params_str, GW_LLM_DEFAULT_TIMEOUT_MS,
                                      &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp)
        return -1;

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -1;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {

        *response_json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return 0;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;

    cJSON *openai = cJSON_CreateObject();
    char idbuf[64];
    snprintf(idbuf, sizeof(idbuf), "chatcmpl-%ld", (long)time(NULL));
    cJSON_AddStringToObject(openai, "id", idbuf);
    cJSON_AddStringToObject(openai, "object", "chat.completion");
    cJSON_AddNumberToObject(openai, "created", (double)time(NULL));
    cJSON_AddStringToObject(openai, "model", (model && model[0]) ? model : ctx->default_model);
    cJSON *choices_out = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    if (choice0) {
        cJSON *content = cJSON_GetObjectItem(choice0, "content");
        cJSON_AddStringToObject(message, "content",
                                cJSON_IsString(content) ? content->valuestring : "");
        cJSON *tool_calls = cJSON_GetObjectItem(choice0, "tool_calls");
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            cJSON_AddItemToObject(message, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        }
        cJSON *reason = cJSON_GetObjectItem(choice0, "finish_reason");
        cJSON_AddStringToObject(choice, "finish_reason",
                                cJSON_IsString(reason) ? reason->valuestring : "stop");
    } else {
        cJSON_AddStringToObject(message, "content", "");
        cJSON_AddStringToObject(choice, "finish_reason", "stop");
    }
    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddItemToArray(choices_out, choice);
    cJSON_AddItemToObject(openai, "choices", choices_out);
    if (result) {
        cJSON *usage = cJSON_GetObjectItem(result, "usage");
        if (cJSON_IsObject(usage)) {
            cJSON_AddItemToObject(openai, "usage", cJSON_Duplicate(usage, 1));
        }
    }

    *response_json = cJSON_PrintUnformatted(openai);
    cJSON_Delete(openai);
    cJSON_Delete(root);
    return *response_json ? 0 : -1;
}

/**
 * @brief A2A task backend: task -> sched_d.schedule_task
 *
 * The output is the scheduling-result JSON
 * (selected_agent_id/confidence/estimated_time_ms), embedded directly into the
 * A2A task response's output field.
 */
int gw_biz_sched_schedule(const char *task_id, const char *task_type, const char *input_json,
                          char **output_json, void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *output_json = NULL;
    if (!ctx || !task_id) {
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON *task = cJSON_CreateObject();
    cJSON_AddStringToObject(task, "task_id", task_id);
    char desc[512];
    snprintf(desc, sizeof(desc), "A2A delegated task (type=%s)", task_type ? task_type : "unknown");
    cJSON_AddStringToObject(task, "task_description", desc);
    cJSON_AddNumberToObject(task, "priority", 0);
    cJSON_AddNumberToObject(task, "timeout_ms", 30000);

    if (input_json && input_json[0]) {
        cJSON *input = cJSON_Parse(input_json);
        if (input) {
            cJSON_AddItemToObject(task, "input", input);
        }
    }
    cJSON_AddItemToObject(params, "task", task);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    /* 架构约束 2026-08-25 "必须走 syscall": sched.schedule_task 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t svc_rc = airy_sys_svc_call("sched", "schedule_task", params_str,
                                          GW_TOOL_TIMEOUT_MS, &resp);
    AIRY_FREE(params_str);
    if (svc_rc != AIRY_SUCCESS || !resp)
        return -1;

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -1;

    int rc = -1;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    cJSON *result = err ? NULL : cJSON_GetObjectItem(root, "result");
    if (result) {
        *output_json = cJSON_PrintUnformatted(result);
        rc = *output_json ? 0 : -1;
    } else {
        cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        const char *m =
            (err && cJSON_IsString(msg) && msg->valuestring) ? msg->valuestring : "schedule failed";
        size_t n = strlen(m) + 32;
        char *ebuf = (char *)AIRY_MALLOC(n);
        if (ebuf) {
            snprintf(ebuf, n, "{\"error\":\"%s\"}", m);
            *output_json = ebuf;
        }
    }
    cJSON_Delete(root);
    return rc;
}
