// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_forward.c
 * @brief Gateway namespace forwarding: L2 protocol client.
 *
 * Acts as the gateway -> daemon L2 service-protocol client
 * (<daemon>.<method>), providing a unified Unix-socket JSON-RPC call
 * (gw_svc_call) and the namespace forwarding handlers.
 *
 * 0.1.6 P1-4 收敛：外部可调用方法的枚举/白名单统一由能力注册表
 * （gateway_cap_registry.h，cap_key 单一权威源）承载，本文件不再维护
 * 任何方法清单；未登记能力在 gateway_business_handler.c 主派发处
 * fail-closed 拒绝（-32601），防止任意方法透传。
 *
 * Split from gateway_business_handler.c (single responsibility: namespace
 * forwarding).
 */

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"
#include "daemon_security.h"

#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

char *jsonrpc_error(int code, const char *msg, const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp)
        return NULL;
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg ? msg : "Unknown error");
    cJSON_AddItemToObject(resp, "error", err);

    if (id && !cJSON_IsNull(id)) {
        if (cJSON_IsString(id)) {
            cJSON_AddStringToObject(resp, "id", id->valuestring);
        } else if (cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(resp, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(resp, "id");
        }
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return out;
}

/**
 * @brief Generic daemon internal service call (Unix socket JSON-RPC)
 *
 * Builds {"jsonrpc":"2.0","method":<method>,"params":<params_json>,"id":1},
 * sends it to the target daemon socket and blocks until the full JSON response
 * is read. The gateway acts as a client of the L2 service protocol
 * (<daemon>.<method>) to call each daemon; daemons need no knowledge of the
 * external protocol.
 *
 * @param sock_path   Target daemon socket path
 * @param method      Internal service method (e.g. "spawn"/"invoke"/"write")
 * @param params_json Method params JSON string (NULL/empty -> "{}")
 * @param timeout_ms  Receive timeout (ms)
 * @return Response JSON string (AIRY_MALLOC, caller AIRY_FREE), or NULL on failure
 */
char *gw_svc_call(const char *sock_path, const char *method, const char *params_json,
                  int timeout_ms)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        close(fd);
        return NULL;
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", method);
    if (params_json && params_json[0]) {
        cJSON *p = cJSON_Parse(params_json);
        cJSON_AddItemToObject(req, "params", p ? p : cJSON_CreateObject());
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        close(fd);
        return NULL;
    }

    size_t len = strlen(req_str);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_str + sent, len - sent, 0);
        if (n <= 0) {
            AIRY_FREE(req_str);
            close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }
    AIRY_FREE(req_str);

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        close(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    close(fd);
    return resp;
#else
    /* Windows：daemon 统一走 TCP 回环（daemon_main.h parse_args 强制），
     * sock_path 参数约定为 "host:port"（如 "127.0.0.1:8086"），与
     * daemon_rpc_client 及 gateway 的 AIRY_LLM_TCP_ADDR/PORT 约定一致。 */
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return NULL;

    char host[128];
    char port_str[16];
    const char *colon = sock_path ? strrchr(sock_path, ':') : NULL;
    if (!colon || colon == sock_path || (size_t)(colon - sock_path) >= sizeof(host) ||
        strlen(colon + 1) >= sizeof(port_str)) {
        closesocket(fd);
        return NULL;
    }
    size_t host_len = (size_t)(colon - sock_path);
    AIRY_MEMCPY(host, sock_path, host_len);
    host[host_len] = '\0';
    AIRY_STRNCPY_TERM(port_str, colon + 1, sizeof(port_str));
    struct sockaddr_in addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(port_str));
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
        addr.sin_addr.s_addr = INADDR_LOOPBACK;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return NULL;
    }

    int timeout_ms_win = timeout_ms > 0 ? timeout_ms : GW_LLM_DEFAULT_TIMEOUT_MS;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms_win,
               sizeof(timeout_ms_win));

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        closesocket(fd);
        return NULL;
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", method);
    if (params_json && params_json[0]) {
        cJSON *p = cJSON_Parse(params_json);
        cJSON_AddItemToObject(req, "params", p ? p : cJSON_CreateObject());
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        closesocket(fd);
        return NULL;
    }

    size_t len = strlen(req_str);
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, req_str + sent, (int)(len - sent), 0);
        if (n <= 0) {
            AIRY_FREE(req_str);
            closesocket(fd);
            return NULL;
        }
        sent += (size_t)n;
    }
    AIRY_FREE(req_str);

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        closesocket(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
                closesocket(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                closesocket(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    closesocket(fd);
    return resp;
#endif
}

/**
 * @brief ACL check for tool execution from external protocols
 *
 * Fail-closed: daemon_check_tool_permission DENYs any rule not registered for
 * (agent_id, tool_name). Default rules are registered at startup in main.c
 * (fs_read/fs_write/fs_list allow; shell_run follows the
 * AIRY_GATEWAY_ACL_ALLOW_SHELL env var, deny by default).
 *
 * @param tool_name Tool name
 * @return 0 allowed, non-zero denied
 */
int gw_acl_check_tool(const char *tool_name)
{
    if (!tool_name)
        return -1;
    int rc = daemon_check_tool_permission(GW_EXTERNAL_AGENT_ID, tool_name, "execute");
    if (rc != 0) {
        AIRY_LOG_WARN("gateway ACL DENY: agent=%s tool=%s (fail-closed)", GW_EXTERNAL_AGENT_ID,
                 tool_name);
        return -1;
    }
    return 0;
}

/**
 * @brief Namespace method forwarding: gateway JSON-RPC <ns>.<method> ->
 *        daemon <method>
 *
 * Same pass-through mode as handle_mem_call: params/response are forwarded
 * as-is, the response id is rewritten to the request id.
 *
 * 0.1.6 P1-4：方法存在性校验已由主派发经能力注册表（gw_cap_find）完成，
 * 本函数仅做命名空间前缀的防御性校验（防内部误用/透传格式错误）。
 *
 * @param rule Forwarding rule (ns/timeout，由能力注册表派生)
 * @return Complete JSON-RPC response string from the target daemon
 *         (AIRY_MALLOC), or an error response on failure
 */
char *handle_ns_forward(cJSON *root, const gw_ns_forward_rule_t *rule)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    const char *method_str = cJSON_IsString(method) ? method->valuestring : NULL;
    if (!method_str || !rule || !rule->ns)
        return jsonrpc_error(-32601, "Method not found", id);

    size_t ns_len = strlen(rule->ns);
    /* 裸命名空间（如 "llm"）+ "." 前缀防御性校验 */
    if (strncmp(method_str, rule->ns, ns_len) != 0 || method_str[ns_len] != '.')
        return jsonrpc_error(-32601, "Method not found", id);
    const char *inner = method_str + ns_len + 1;
    if (!*inner)
        return jsonrpc_error(-32601, "Method not found", id);

    cJSON *params = cJSON_GetObjectItem(root, "params");
    char *params_str = params ? cJSON_PrintUnformatted(params) : AIRY_STRDUP("{}");
    if (!params_str)
        return jsonrpc_error(-32603, "Out of memory", id);

    /* 架构约束 2026-08-25 "必须走 syscall": 命名空间转发统一经 SYS_SVC_CALL
     * 派发（钩子按命名空间路由到对应 daemon 端点，见 gateway_biz_svcdispatch.c）。 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call(rule->ns, inner, params_str, (uint32_t)rule->timeout_ms,
                                      &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp)
        return jsonrpc_error(-32603, "Service unreachable", id);

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot)
        return jsonrpc_error(-32603, "Service returned invalid response", id);

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
 * @brief mem.* forwarding: gateway JSON-RPC -> mem_d (params/response pass-through)
 *
 * 0.1.6 P1-4：mem.* 方法枚举由能力注册表（GW_CAP_KIND_MEM）承载，主派发
 * 已保证方法已登记；本函数直接转发内层方法名（<ns>.<method> 的 method）。
 *
 * Env-gated by AIRY_GATEWAY_MEM_PUBLIC (default true: internal memory service
 * traffic passes; false disables external mem access without affecting the TUI
 * local JSONL).
 */
char *handle_mem_call(cJSON *root)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    const char *method_str = cJSON_IsString(method) ? method->valuestring : NULL;
    cJSON *params = cJSON_GetObjectItem(root, "params");

    const char *mem_method = NULL;
    if (method_str && strncmp(method_str, "mem.", 4) == 0)
        mem_method = method_str + 4;
    if (!mem_method || !*mem_method)
        return jsonrpc_error(-32601, "Method not found", id);

    const char *pub = getenv("AIRY_GATEWAY_MEM_PUBLIC");
    if (pub && (strcmp(pub, "false") == 0 || strcmp(pub, "0") == 0)) {
        return jsonrpc_error(-32001, "Memory service access disabled", id);
    }

    char *params_str = NULL;
    if (params) {
        params_str = cJSON_PrintUnformatted(params);
    } else {
        params_str = AIRY_STRDUP("{}");
    }
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    /* 架构约束 2026-08-25 "必须走 syscall": mem.* 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("mem", mem_method, params_str, GW_TOOL_TIMEOUT_MS, &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        return jsonrpc_error(-32603, "Memory service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Memory service returned invalid response", id);
    }
    /* JSON-RPC 2.0 compliance: the response id must match the request id.
     * mem_d echoes the internal id=1 used by gw_svc_call; without rewriting,
     * concurrent requests cannot be correlated to their originals (client id
     * validation would fail). */
    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *mem_id = cJSON_GetObjectItem(rroot, "id");
    if (mem_id)
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
 * @brief llm.list_models forwarding: gateway JSON-RPC -> llm_d list_models
 *
 * Returns all models from the llm_d provider registry plus
 * default_model/default_provider, for CLI/TUI model configuration (read-only,
 * no params, no API key needed). The response id is rewritten to the request
 * id (same concurrency compliance as handle_mem_call).
 */
char *handle_llm_list_models(cJSON *root, const gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */

    /* 架构约束 2026-08-25 "必须走 syscall": llm.list_models 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("llm", "list_models", "{}", GW_LLM_DEFAULT_TIMEOUT_MS,
                                      &resp);
    if (rc != AIRY_SUCCESS || !resp) {
        return jsonrpc_error(-32603, "LLM service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "LLM service returned invalid response", id);
    }

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *llm_id = cJSON_GetObjectItem(rroot, "id");
    if (llm_id)
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
 * @brief tool.pending / tool.approve forwarding: gateway JSON-RPC -> tool_d
 *
 * P0 interactive permission approval (Claude Code style permission prompt):
 * external tool.pending -> tool_d "pending"; external tool.approve ->
 * tool_d "approve". params/response pass through, the response id is rewritten
 * to the request id (same concurrency compliance as handle_mem_call).
 */
char *handle_tool_approval_call(cJSON *root, const gateway_business_ctx_t *ctx,
                                const char *tool_method)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */

    if (strcmp(tool_method, "approve") == 0) {
        const cJSON *req_id = params ? cJSON_GetObjectItem(params, "request_id") : NULL;
        const cJSON *decision = params ? cJSON_GetObjectItem(params, "decision") : NULL;
        if (!cJSON_IsString(req_id) || !req_id->valuestring || !req_id->valuestring[0] ||
            !cJSON_IsString(decision) || !decision->valuestring || !decision->valuestring[0]) {
            return jsonrpc_error(-32602, "Invalid params: request_id and decision required", id);
        }
    }

    char *params_str = NULL;
    if (params) {
        params_str = cJSON_PrintUnformatted(params);
    } else {
        params_str = AIRY_STRDUP("{}");
    }
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    /* 架构约束 2026-08-25 "必须走 syscall": tool.pending/approve 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("tool", tool_method, params_str, GW_TOOL_TIMEOUT_MS, &resp);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        return jsonrpc_error(-32603, "Tool service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Tool service returned invalid response", id);
    }

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *tool_id = cJSON_GetObjectItem(rroot, "id");
    if (tool_id)
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
