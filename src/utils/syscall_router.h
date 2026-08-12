/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file syscall_router.h
 * @brief Syscall router interface.
 *
 * Routes JSON-RPC requests to syscalls uniformly; shared by the
 * HTTP/WebSocket/Stdio gateways.
 */

/* @owner: team-B */
#ifndef GATEWAY_SYSCALL_ROUTER_H
#define GATEWAY_SYSCALL_ROUTER_H

#include "airy_rt.h"

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief Route a system call request
 *
  * Routes a JSON-RPC method name and params to the corresponding syscall function.
 *
  * @param[in] method Method name (e.g. "airy_sys_task_submit")
  * @param[in] params Parameter object
  * @param[in] request_id Request ID (may be NULL)
  * @return JSON response string; caller must free()
 */
char *gateway_syscall_route(const char *method, cJSON *params, cJSON *request_id);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_SYSCALL_ROUTER_H */
