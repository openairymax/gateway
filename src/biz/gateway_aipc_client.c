// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_aipc_client.c
 * @brief Gateway southbound A-IPC unified client face (0.1.16 B3, design §4).
 *
 * Single owner of every socket(AF_UNIX) creation under gateway/src (gate N2).
 * The REQUEST/RESPONSE implementation is the migrated gw_svc_call body
 * (blueprint 8.3.3 L2-first + socket/TCP fallback); gw_svc_call itself is now
 * a thin wrapper (gateway_biz_forward.c) so zero call sites changed.
 *
 * STREAM/EVENT are the transition form from design §4.2: connect + send is
 * centralized here while the B1 L2 STREAM/EVENT client mapping matures; the
 * fd-returning contract keeps the MHD pull-model readers and the PEP
 * fail-open observer untouched.
 */

#include "gateway_aipc_client.h"

#include "logging.h"
#include "platform.h"
#include "daemon_l1_server.h" /* blueprint 8.3.3: L2 first path in gw_aipc_call */

#include "airy_memory.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* fd 类型随传输分支而异（POSIX int / WIN32 SOCKET），关闭助手同理成对定义 */
#ifndef _WIN32
static void gw_aipc_sock_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
#else
static void gw_aipc_sock_close(SOCKET fd)
{
    if (fd != INVALID_SOCKET)
        closesocket(fd);
}
#endif

/* ── REQUEST/RESPONSE ──────────────────────────────────────────────── */

char *gw_aipc_call(const char *sock_path, const char *method,
                   const char *params_json, int timeout_ms)
{
    /* Blueprint 8.3.3 grey rollout: when the ns transport switch resolves to
     * "corekern" (the only case channel_for_socket succeeds), serve the call
     * over the L2 channel. The _resp variant returns the complete JSON-RPC
     * response — daemon error replies included verbatim — so the return
     * contract matches the socket path below bit-for-bit. Any L2 miss falls
     * through to the socket path: NOT_FOUND from channel_for_socket (switch
     * off) skips the block entirely, ENOENT from connect (switch on but
     * bridge not mounted yet: the grey coexistence norm) fails the call and
     * drops to the fallback, as does any other transport loss. */
    char channel[64];
    if (sock_path && daemon_l2_channel_for_socket(sock_path, channel, sizeof(channel)) == 0) {
        char *l2_resp = NULL;
        uint32_t l2_timeout = timeout_ms > 0 ? (uint32_t)timeout_ms : 0;
        if (daemon_l2_rpc_call_resp(channel, method, params_json, l2_timeout, &l2_resp) ==
            AIRY_SUCCESS)
            return l2_resp;
        /* transport on but bridge not mounted: fall back to the socket path */
    }

    /* JSON-RPC request envelope shared by both transports below */
    char *req_str = NULL;
    {
        cJSON *req = cJSON_CreateObject();
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
        req_str = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!req_str)
            return NULL;
    }

#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        AIRY_FREE(req_str);
        return NULL;
    }
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        AIRY_FREE(req_str);
        close(fd);
        return NULL;
    }

    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#else
    /* Windows：daemon 统一走 TCP 回环（daemon_main.h parse_args 强制），
     * sock_path 参数约定为 "host:port"（如 "127.0.0.1:8086"），与
     * daemon_rpc_client 及 gateway 的 AIRY_LLM_TCP_ADDR/PORT 约定一致。 */
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        AIRY_FREE(req_str);
        return NULL;
    }

    char host[128];
    char port_str[16];
    const char *colon = sock_path ? strrchr(sock_path, ':') : NULL;
    if (!colon || colon == sock_path || (size_t)(colon - sock_path) >= sizeof(host) ||
        strlen(colon + 1) >= sizeof(port_str)) {
        AIRY_FREE(req_str);
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
        AIRY_FREE(req_str);
        closesocket(fd);
        return NULL;
    }

    int timeout_ms_win = timeout_ms > 0 ? timeout_ms : 90000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms_win,
               sizeof(timeout_ms_win));
#endif

    size_t len = strlen(req_str);
    size_t sent = 0;
    while (sent < len) {
#ifndef _WIN32
        ssize_t n = send(fd, req_str + sent, len - sent, 0);
#else
        int n = send(fd, req_str + sent, (int)(len - sent), 0);
#endif
        if (n <= 0) {
            AIRY_FREE(req_str);
            gw_aipc_sock_close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }
    AIRY_FREE(req_str);

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        gw_aipc_sock_close(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
#ifndef _WIN32
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
#else
        int n = recv(fd, buf, sizeof(buf), 0);
#endif
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_AIPC_MAX_RESP) {
                AIRY_FREE(resp);
                gw_aipc_sock_close(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                gw_aipc_sock_close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    gw_aipc_sock_close(fd);
    return resp;
}

/* ── STREAM（过渡态：连接 + 发送，分片读循环留在调用方）──────────────── */

int gw_aipc_stream(const char *sock_path, const char *req_json, int poll_timeout_s)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (poll_timeout_s > 0) {
        struct timeval tv = {poll_timeout_s, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    size_t len = strlen(req_json);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_json + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)n;
    }
    return fd;
#else
    (void)sock_path;
    (void)req_json;
    (void)poll_timeout_s;
    return -1;
#endif
}

/* ── EVENT（过渡态：连接 + 握手发送，帧循环/重连留在调用方）──────────── */

int gw_aipc_subscribe(const char *sock_path, const char *handshake,
                      size_t handshake_len)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    size_t sent = 0;
    while (sent < handshake_len) {
        ssize_t n = send(fd, handshake + sent, handshake_len - sent, 0);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)n;
    }
    return fd;
#else
    (void)sock_path;
    (void)handshake;
    (void)handshake_len;
    return -1;
#endif
}
