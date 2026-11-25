// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_aipc_client.c
 * @brief Gateway southbound A-IPC unified client face.
 *
 * Single owner of every daemon-plane socket connection under gateway/src
 * (gate N2). Transport ceremony — endpoint parsing, connect, receive timeout,
 * whole-frame send — lives in commons/platform (platform_sock_conn.c); this
 * file carries only the A-IPC wire policy (JSON-RPC envelope, response cap).
 * POSIX talks AF_UNIX, Windows talks TCP loopback; neither branch is visible
 * here, so the three entry points share one code path on every platform.
 *
 * STREAM/EVENT keep the fd-returning contract (design §4.2): MHD pull-model
 * readers and the PEP fail-open observer own their chunked read loops, idle
 * deadlines and reconnect policy.
 */

#include "gateway_aipc_client.h"

#include "logging.h"
#include "platform.h"
#include "daemon_platform_ext.h"

#include "airy_memory.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* REQUEST/RESPONSE 的线上信封：id 固定为 1（单请求单响应连接，无需复用
 * 关联号），params 非对象时回落空对象，避免对端解析失败。 */
static char *gw_aipc_req(const char *method, const char *params_json)
{
    cJSON *req;
    char *out;

    if (!method || !method[0])
        return NULL;
    req = cJSON_CreateObject();
    if (!req)
        return NULL;
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", method);
    if (params_json && params_json[0]) {
        cJSON *p = cJSON_Parse(params_json);
        cJSON_AddItemToObject(req, "params", p ? p : cJSON_CreateObject());
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }
    out = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return out;
}

/* ── REQUEST/RESPONSE ──────────────────────────────────────────────── */

char *gw_aipc_call(const char *sock_path, const char *method,
                   const char *params_json, int timeout_ms)
{
    char *req_str = gw_aipc_req(method, params_json);
    airy_sock_t fd;
    char *resp;
    char buf[4096];
    size_t cap = 65536;
    size_t used = 0;

    if (!req_str)
        return NULL;
    fd = airy_sock_conn_ep(sock_path, 0);
    if (fd == AIRY_INVALID_SOCKET) {
        AIRY_LOG_WARN("aipc: connect failed (%s)", sock_path ? sock_path : "");
        AIRY_FREE(req_str);
        return NULL;
    }
    if (timeout_ms <= 0)
        timeout_ms = GW_AIPC_DEFAULT_TIMEOUT_MS;
    if (airy_sock_rcv_tmo(fd, (uint32_t)timeout_ms) != AIRY_SUCCESS) {
        AIRY_FREE(req_str);
        airy_sock_close(fd);
        return NULL;
    }
    if (airy_sock_send_all(fd, req_str, strlen(req_str)) != AIRY_SUCCESS) {
        AIRY_LOG_WARN("aipc: send failed (%s)", method);
        AIRY_FREE(req_str);
        airy_sock_close(fd);
        return NULL;
    }
    AIRY_FREE(req_str);

    resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        airy_sock_close(fd);
        return NULL;
    }
    resp[0] = '\0';
    for (;;) {
        ssize_t n = airy_sock_recv_block(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > GW_AIPC_MAX_RESP) {
            AIRY_FREE(resp);
            airy_sock_close(fd);
            return NULL;
        }
        if (used + (size_t)n + 1 > cap) {
            size_t need = used + (size_t)n + 1;
            size_t new_cap = cap * 2;
            char *np;
            if (new_cap < need)
                new_cap = need;
            if (new_cap > GW_AIPC_MAX_RESP)
                new_cap = GW_AIPC_MAX_RESP;
            np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                airy_sock_close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    airy_sock_close(fd);
    if (used == 0) {
        AIRY_LOG_WARN("aipc: peer closed without payload (%s)", sock_path ? sock_path : "");
        AIRY_FREE(resp);
        return NULL;
    }
    return resp;
}

/* ── STREAM（过渡态：连接 + 发送，分片读循环留在调用方）──────────────── */

airy_sock_t gw_aipc_stream(const char *sock_path, const char *req_json, int poll_timeout_s)
{
    airy_sock_t fd;

    if (!req_json)
        return AIRY_INVALID_SOCKET;
    fd = airy_sock_conn_ep(sock_path, 0);
    if (fd == AIRY_INVALID_SOCKET)
        return AIRY_INVALID_SOCKET;
    if (poll_timeout_s > 0)
        airy_sock_rcv_tmo(fd, (uint32_t)poll_timeout_s * 1000u);
    if (airy_sock_send_all(fd, req_json, strlen(req_json)) != AIRY_SUCCESS) {
        airy_sock_close(fd);
        return AIRY_INVALID_SOCKET;
    }
    return fd;
}

/* ── EVENT（过渡态：连接 + 握手发送，帧循环/重连留在调用方）──────────── */

airy_sock_t gw_aipc_subscribe(const char *sock_path, const char *handshake,
                              size_t handshake_len)
{
    airy_sock_t fd;

    if (!handshake)
        return AIRY_INVALID_SOCKET;
    fd = airy_sock_conn_ep(sock_path, 0);
    if (fd == AIRY_INVALID_SOCKET)
        return AIRY_INVALID_SOCKET;
    if (airy_sock_send_all(fd, handshake, handshake_len) != AIRY_SUCCESS) {
        airy_sock_close(fd);
        return AIRY_INVALID_SOCKET;
    }
    return fd;
}
