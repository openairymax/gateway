// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_business_handler.c
 * @brief Gateway business-request handler: ctx lifecycle + main dispatch +
 *        protocol entry.
 *
 * SEC-017 compliant: all features are real implementations, no stubs.
 * Chain: HTTP JSON-RPC agent.run -> dual-think/orchestration -> returns
 * the chat result.
 *
 * Split from the original 2608-line monolith by single responsibility
 * (2026-08-11):
 *   - gateway_biz_forward.c  namespace forwarding (L2 protocol client)
 *   - gateway_biz_llm.c      LLM calls + tool loop (ReAct)
 *   - gateway_biz_agent.c    agent.run orchestration (dual-think injection + cancel)
 *   - gateway_biz_backend.c  MCP/OpenAI/A2A protocol backends
 *   - gateway_biz_hall.c     hall.* task board / event stream / chain (in-gateway)
 *   - gateway_cap_registry.c unified capability registry (0.1.6 P1-4: cap_key
 *                            single source of truth for external capabilities)
 * This file keeps: ctx lifecycle, JSON-RPC main dispatch, protocol-detection entry.
 */

#include "gateway_business_handler.h"

#include "gateway_biz_internal.h"

#include "gateway_cap_registry.h"

#include "logging.h"
#include "platform.h"
#include "gateway_protocol_router.h"
#include "daemon_security.h"
#include "http_gateway.h"

#include "svc_model_defaults.h"

#include "daemon_heapstore_bootstrap.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Resolve the daemon endpoint: <ENV_NAME> override ->
 * airy_runtime_dir()/<sock_name> -> <sock_name>. Kept consistent with the
 * daemon-side single source of truth: airy_runtime_dir() resolves $AIRY_HOME/run,
 * defaulting to ~/.airymaxrt/run.
 *
 * Windows: daemon IPC 走 TCP 回环（daemon_main.h parse_args 强制），
 * 端点约定为 "host:port"，端口与各 daemon DEFAULT_TCP_PORT 对齐。 */
static void gw_resolve_daemon_sock(char *out, size_t out_size, const char *env_name,
                                   const char *sock_name)
{
    const char *env = getenv(env_name);
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_size);
        return;
    }
#ifdef _WIN32
    static const struct { const char *ns; const char *ep; } WIN_SOCK_TCP[] = {
        {"llm.sock", "127.0.0.1:8080"},     {"tool.sock", "127.0.0.1:8081"},
        {"market.sock", "127.0.0.1:8082"},   {"sched.sock", "127.0.0.1:8083"},
        {"notify.sock", "127.0.0.1:8084"},   {"mem.sock", "127.0.0.1:8085"},
        {"agent.sock", "127.0.0.1:8086"},    {"a2a.sock", "127.0.0.1:8087"},
        {"info.sock", "127.0.0.1:8088"},     {"cupolas.sock", "127.0.0.1:8089"},
        {"think.sock", "127.0.0.1:8090"},    {"observe.sock", "127.0.0.1:8091"},
        {"plugin.sock", "127.0.0.1:8092"},   {"hook.sock", "127.0.0.1:8093"},
        {"channel.sock", "127.0.0.1:8094"},  {"monit.sock", "127.0.0.1:9090"},
    };
    for (size_t i = 0; i < sizeof(WIN_SOCK_TCP) / sizeof(WIN_SOCK_TCP[0]); i++) {
        if (strcmp(sock_name, WIN_SOCK_TCP[i].ns) == 0) {
            AIRY_STRNCPY_TERM(out, WIN_SOCK_TCP[i].ep, out_size);
            return;
        }
    }
    AIRY_STRNCPY_TERM(out, sock_name, out_size);
#else
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir) {
        snprintf(out, out_size, "%s/%s", run_dir, sock_name);
    } else {
        AIRY_STRNCPY_TERM(out, sock_name, out_size);
    }
#endif
}

gateway_business_ctx_t *gateway_business_ctx_create(void)
{
    gateway_business_ctx_t *ctx =
        (gateway_business_ctx_t *)AIRY_CALLOC(1, sizeof(gateway_business_ctx_t));
    if (!ctx)
        return NULL;

    /* Each daemon socket: <DAEMON>_SOCK env -> $AIRY_RUNTIME_DIR/<name>.sock ->
     * $AIRY_HOME/run/<name>.sock */
    gw_resolve_daemon_sock(ctx->llm_sock_path, sizeof(ctx->llm_sock_path), "AIRY_LLM_SOCK",
                           "llm.sock");
    gw_resolve_daemon_sock(ctx->tool_sock_path, sizeof(ctx->tool_sock_path), "AIRY_TOOL_SOCK",
                           "tool.sock");
    gw_resolve_daemon_sock(ctx->agent_sock_path, sizeof(ctx->agent_sock_path), "AIRY_AGENT_SOCK",
                           "agent.sock");
    gw_resolve_daemon_sock(ctx->mem_sock_path, sizeof(ctx->mem_sock_path), "AIRY_MEM_SOCK",
                           "mem.sock");
    gw_resolve_daemon_sock(ctx->sched_sock_path, sizeof(ctx->sched_sock_path), "AIRY_SCHED_SOCK",
                           "sched.sock");
    gw_resolve_daemon_sock(ctx->think_sock_path, sizeof(ctx->think_sock_path), "AIRY_THINK_SOCK",
                           "think.sock");
    gw_resolve_daemon_sock(ctx->a2a_sock_path, sizeof(ctx->a2a_sock_path), "AIRY_A2A_SOCK",
                           "a2a.sock");
    gw_resolve_daemon_sock(ctx->plugin_sock_path, sizeof(ctx->plugin_sock_path), "AIRY_PLUGIN_SOCK",
                           "plugin.sock");
    gw_resolve_daemon_sock(ctx->info_sock_path, sizeof(ctx->info_sock_path), "AIRY_INFO_SOCK",
                           "info.sock");
    gw_resolve_daemon_sock(ctx->notify_sock_path, sizeof(ctx->notify_sock_path), "AIRY_NOTIFY_SOCK",
                           "notify.sock");
    gw_resolve_daemon_sock(ctx->observe_sock_path, sizeof(ctx->observe_sock_path),
                           "AIRY_OBSERVE_SOCK", "observe.sock");
    gw_resolve_daemon_sock(ctx->market_sock_path, sizeof(ctx->market_sock_path), "AIRY_MARKET_SOCK",
                           "market.sock");
    gw_resolve_daemon_sock(ctx->hook_sock_path, sizeof(ctx->hook_sock_path), "AIRY_HOOK_SOCK",
                           "hook.sock");
    gw_resolve_daemon_sock(ctx->monit_sock_path, sizeof(ctx->monit_sock_path), "AIRY_MONIT_SOCK",
                           "monit.sock");
    gw_resolve_daemon_sock(ctx->channel_sock_path, sizeof(ctx->channel_sock_path),
                           "AIRY_CHANNEL_SOCK", "channel.sock");
    gw_resolve_daemon_sock(ctx->cupolas_sock_path, sizeof(ctx->cupolas_sock_path),
                           "AIRY_CUPOLAS_SOCK", "cupolas.sock");

    const char *tcp_env = getenv("AIRY_LLM_TCP_ADDR");
    AIRY_STRNCPY_TERM(ctx->llm_tcp_addr, (tcp_env && *tcp_env) ? tcp_env : "127.0.0.1",
                      sizeof(ctx->llm_tcp_addr));
    const char *port_env = getenv("AIRY_LLM_TCP_PORT");
    ctx->llm_tcp_port =
        (port_env && *port_env) ? (uint16_t)atoi(port_env) : GW_LLM_DEFAULT_TCP_PORT;

    /* Default model: env AIRY_AGENT_MODEL > user override
     * $AIRY_CONFIG_DIR/model.yaml global.default_model > built-in default
     * (aligned with model.yaml). Users need not touch the repo SSoT; overriding
     * the global section in $AIRY_HOME/config/model.yaml applies to both
     * gateway and llm_d (same resolution path). */
    const char *model_env = getenv("AIRY_AGENT_MODEL");
    if (model_env && *model_env) {
        AIRY_STRNCPY_TERM(ctx->default_model, model_env, sizeof(ctx->default_model));
    } else {
        char um[128] = {0};
        const char *cfg_dir = airy_config_dir();
        int has_user_cfg = 0;
        if (cfg_dir) {
            char user_path[1024];
            int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
            if (plen > 0 && plen < (int)sizeof(user_path)) {
                FILE *uf = fopen(user_path, "rb");
                if (uf) {
                    fclose(uf);
                    if (svc_model_defaults_from_yaml(user_path, um, sizeof(um), NULL, 0) == 0 &&
                        um[0])
                        has_user_cfg = 1;
                    else {
                        /* No global section: fall back to the simple llm
                         * section model (same semantics as llm_d; the default
                         * model configured under llm: also applies to gateway) */
                        svc_model_llm_config_t llm_cfg;
                        AIRY_MEMSET(&llm_cfg, 0, sizeof(llm_cfg));
                        if (svc_model_defaults_llm_from_yaml(user_path, &llm_cfg) == 0 &&
                            llm_cfg.model[0]) {
                            AIRY_STRNCPY_TERM(um, llm_cfg.model, sizeof(um));
                            has_user_cfg = 1;
                        } else {
                            /* v2 表格格式（2026-08-26）：llm 段缺省时回退
                             * models 表首个条目 */
                            AIRY_MEMSET(&llm_cfg, 0, sizeof(llm_cfg));
                            if (svc_model_defaults_models0_from_yaml(user_path, &llm_cfg) == 0 &&
                                llm_cfg.model[0]) {
                                AIRY_STRNCPY_TERM(um, llm_cfg.model, sizeof(um));
                                has_user_cfg = 1;
                            }
                        }
                    }
                }
            }
        }
        AIRY_STRNCPY_TERM(ctx->default_model, has_user_cfg ? um : GW_LLM_DEFAULT_MODEL,
                          sizeof(ctx->default_model));
    }

    /* 架构约束 2026-08-25 "必须走 syscall": 注入微核心服务统一派发钩子，
     * gateway 对 daemon 的所有派发经 airy_sys_svc_call() (SYS_SVC_CALL)
     * 完成（见 gateway_biz_svcdispatch.c）。 */
    gw_sys_svc_dispatch_init(ctx);

    return ctx;
}

int gateway_business_ctx_set_shutdown_cb(gateway_business_ctx_t *ctx, gateway_shutdown_fn_t cb,
                                         void *user_data)
{
    if (!ctx)
        return AIRY_ERR_INVALID_PARAM;
    ctx->on_shutdown = cb;
    ctx->shutdown_user_data = user_data;
    return AIRY_SUCCESS;
}

void gateway_business_ctx_destroy(gateway_business_ctx_t *ctx)
{
    if (!ctx)
        return;

    gw_sys_svc_dispatch_cleanup();
    AIRY_FREE(ctx);
}

char *gateway_business_handle(void *request, void *user_data)
{
    const char *req = (const char *)request;
    gateway_business_ctx_t *ctx = (gateway_business_ctx_t *)user_data;
    if (!req || !ctx) {
        return jsonrpc_error(-32600, "Invalid request", NULL);
    }

    cJSON *root = cJSON_Parse(req);
    if (!root) {
        return jsonrpc_error(-32700, "Parse error", NULL);
    }

    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method)) {
        cJSON_Delete(root);
        return jsonrpc_error(-32600, "Invalid Request", NULL);
    }

    daemon_heapstore_log("gateway_d", 1, method->valuestring, NULL);

    char *resp = NULL;

    /* L0 传输级方法（非能力，不入能力注册表）：ping / shutdown */
    if (strcmp(method->valuestring, "ping") == 0) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "jsonrpc", "2.0");
        if (id && cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(out, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(out, "id");
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "ok");
        cJSON_AddItemToObject(out, "result", result);
        resp = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
    } else if (strcmp(method->valuestring, "shutdown") == 0) {
        /* Standard L2 method <ns>.shutdown (02-l2-service-protocol.md §6.1:
         * graceful stop). Build the success response first, then invoke the
         * host callback to trigger the real graceful exit (main loop exits),
         * so the response is not cut off by the shutdown. */
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "jsonrpc", "2.0");
        if (id && cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(out, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(out, "id");
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "shutting_down");
        cJSON_AddItemToObject(out, "result", result);
        resp = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        if (ctx->on_shutdown) {
            ctx->on_shutdown(ctx->shutdown_user_data);
        } else {
            AIRY_LOG_WARN("gateway: shutdown requested but no shutdown callback registered");
        }
    } else {
        /* 统一能力网关（0.1.6 P1-4）：cap_key 单一权威源查表分派。
         * 能力注册表（gateway_cap_registry.h）是外部可调用能力的唯一
         * 枚举；未登记能力 fail-closed 拒绝（-32601），防止任意方法透传。 */
        const gw_cap_t *cap = gw_cap_find(method->valuestring);
        if (!cap) {
            gw_cap_emit(method->valuestring, "deny", "unregistered capability");
            cJSON *id = cJSON_GetObjectItem(root, "id");
            resp = jsonrpc_error(-32601, "Method not found", id);
            cJSON_Delete(root);
            return resp;
        }
        /* SSoT 契约版本校验（0.1.6 P1-4）：调用方可经 params.cap_version 声明
         * 期望契约版本；不匹配即拒绝，防止按过时契约调用。未声明不强制。 */
        cJSON *params = cJSON_GetObjectItem(root, "params");
        cJSON *ver = params ? cJSON_GetObjectItem(params, "cap_version") : NULL;
        if (ver && cJSON_IsNumber(ver) && ver->valuedouble > 0 &&
            gw_cap_check_version(cap->cap_key, (int)ver->valuedouble) != 0) {
            gw_cap_emit(cap->cap_key, "deny", "contract version mismatch");
            resp = jsonrpc_error(-32602, "Capability contract version mismatch",
                                 cJSON_GetObjectItem(root, "id"));
            cJSON_Delete(root);
            return resp;
        }
        /* SSoT 权限校验（0.1.6 P1-4）：高敏能力需显式 ACL 授权（fail-closed，
         * 未注册规则即拒绝）；未声明权限的能力默认放行（核心链路不变量）。 */
        const char *perm = gw_cap_perm_for(cap->cap_key);
        if (perm && daemon_check_tool_permission(GW_EXTERNAL_AGENT_ID, perm, "execute") != 0) {
            gw_cap_emit(cap->cap_key, "deny", "permission denied");
            AIRY_LOG_WARN("gateway cap DENY: cap=%s perm=%s (fail-closed)",
                     cap->cap_key, perm);
            resp = jsonrpc_error(-32603, "Permission denied",
                                 cJSON_GetObjectItem(root, "id"));
            cJSON_Delete(root);
            return resp;
        }
        gw_cap_emit(cap->cap_key, "ok", NULL);
        switch (cap->kind) {
        case GW_CAP_KIND_AGENT_RUN:
            resp = (strcmp(cap->method, "run") == 0) ? handle_agent_run(root, ctx)
                                                     : handle_agent_cancel(root, ctx);
            break;
        case GW_CAP_KIND_LLM_LIST:
            resp = handle_llm_list_models(root, ctx);
            break;
        case GW_CAP_KIND_MEM:
            resp = handle_mem_call(root);
            break;
        case GW_CAP_KIND_TOOL_APPROVE:
            resp = handle_tool_approval_call(root, ctx, cap->method);
            break;
        case GW_CAP_KIND_HALL:
            resp = handle_hall_call(root, ctx);
            break;
        case GW_CAP_KIND_FWD: {
            gw_ns_forward_rule_t rule = {cap->ns, gw_cap_ns_timeout(cap->ns)};
            resp = handle_ns_forward(root, &rule);
            break;
        }
        default:
            resp = jsonrpc_error(-32601, "Method not found", cJSON_GetObjectItem(root, "id"));
            break;
        }
    }

    cJSON_Delete(root);
    return resp;
}

static int is_mcp_jsonrpc_method(const char *method)
{
    static const char *mcp_methods[] = {
        "initialize",
        "tools/list",
        "tools/call",
        "resources/list",
        "resources/read",
        "prompts/list",
        "notifications/initialized",
        NULL,
    };
    if (!method)
        return 0;
    for (int i = 0; mcp_methods[i]; i++) {
        if (strcmp(method, mcp_methods[i]) == 0)
            return 1;
    }

    return 0;
}

static int is_a2a_jsonrpc_method(const char *method)
{
    static const char *a2a_methods[] = {
        "tasks/send",   "tasks/get",      "tasks/cancel",       "tasks/pushNotification",
        "message/send", "agent-card/get", "agent/getAgentCard", NULL,
    };
    if (!method)
        return 0;
    for (int i = 0; a2a_methods[i]; i++) {
        if (strcmp(method, a2a_methods[i]) == 0)
            return 1;
    }
    /* Note: must not hijack by the "a2a." prefix — a2a.* is also a JSON-RPC
     * business namespace (gateway -> a2a_d forwarding chain, e.g.
     * a2a.discover_agents). The earlier prefix matching misrouted business
     * methods to the A2A protocol handler (proto=A2A) causing -32603. */
    return 0;
}

char *gateway_protocol_entry(void *request, void *user_data)
{
    const gateway_entry_ctx_t *ectx = (const gateway_entry_ctx_t *)user_data;
    if (!request || !ectx || !ectx->biz_ctx || !ectx->router) {
        return jsonrpc_error(-32600, "Invalid request", NULL);
    }

    /* HTTP transport wraps non-JSON-RPC bodies in gateway_http_request_t to
     * preserve method/path; other transports (stdio/ws) pass the plain body.
     * Path is essential for OpenAI routing (/v1/embeddings has no "messages"
     * field in its body, so body-only detection would misclassify it). */
    const char *method = "POST";
    const char *path = NULL;
    const char *body = (const char *)request;
    /* HTTP transport passes gateway_http_request_t (first byte '1' of the
     * "HTT1" magic); plain JSON bodies (stdio/ws) start with '{'/'['. Check
     * the first byte before the 4-byte magic compare to avoid over-read on
     * very short body strings. */
    const unsigned char *req0 = (const unsigned char *)request;
    if (req0[0] != '{' && req0[0] != '[') {
        uint32_t magic = 0;
        __builtin_memcpy(&magic, request, sizeof(uint32_t));
        if (magic == GATEWAY_HTTP_REQUEST_MAGIC) {
            const gateway_http_request_t *http_req = (const gateway_http_request_t *)request;
            method = http_req->method ? http_req->method : "POST";
            path = http_req->path;
            body = http_req->body ? http_req->body : "";
        }
    }

    gw_proto_detect_result_t proto = gw_proto_detect(NULL, path, body);

    if (proto == GW_PROTO_DETECT_JSONRPC) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            cJSON *m = cJSON_GetObjectItem(root, "method");
            if (cJSON_IsString(m)) {
                if (is_mcp_jsonrpc_method(m->valuestring)) {
                    proto = GW_PROTO_DETECT_MCP;
                } else if (is_a2a_jsonrpc_method(m->valuestring)) {
                    proto = GW_PROTO_DETECT_A2A;
                }
            }
            cJSON_Delete(root);
        }
    }

    if (proto == GW_PROTO_DETECT_MCP || proto == GW_PROTO_DETECT_OPENAI ||
        proto == GW_PROTO_DETECT_A2A) {
        char *resp = NULL;
        int rc = gw_proto_router_route((gw_proto_router_t *)ectx->router, proto, method, path, body,
                                       &resp);
        if (rc != 0 || !resp) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Protocol handler failed: proto=%d rc=%d", (int)proto, rc);
            return jsonrpc_error(-32603, msg, NULL);
        }
        return resp;
    }

    return gateway_business_handle((void *)body, ectx->biz_ctx);
}
