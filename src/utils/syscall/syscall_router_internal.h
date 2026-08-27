// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_internal.h
 * @brief Macros, types and cross-file declarations shared by syscall_router split files.
 */

#ifndef GATEWAY_SYSCALL_ROUTER_INTERNAL_H
#define GATEWAY_SYSCALL_ROUTER_INTERNAL_H

#include "syscall_router.h"

#include "daemon_rpc_client.h"
#include "error.h"
#include "error.h"
#include "jsonrpc.h"
#include "logging.h"
#include "airy_memory.h"
#include "platform.h"
#include "string_compat.h"
#include "svc_logger.h"
#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include <cjson/cJSON.h>

#include <airymax/sched.h>

/* Phase 3: executor consolidation - daemon Unix socket path
 *
  * airy_sys_memory_* / airy_sys_agent_* moved to the mem_d / agent_d daemons;
  * this file only keeps the thin IPC forwarding. Socket paths match the daemons
  * (see daemons/mem_d/src/main.c and daemons/agent_d/src/main.c). */
/* Daemon socket paths must be resolved at runtime (airy_runtime_dir_socket),
 * NOT baked as compile-time AIRY_RUNTIME_DIR macros: daemons listen under
 * $AIRY_HOME/run, which only the runtime resolver knows. A compile-time path
 * (default /tmp/agentrt) makes every airy_sys_* forwarding call connect to a
 * non-existent socket when AIRY_HOME is set (root cause of syscall domains
 * appearing "unreachable" in deployed environments). */
#ifndef AIRY_MEM_D_SOCKET
#define AIRY_MEM_D_SOCKET airy_runtime_dir_socket("mem.sock")
#endif
#ifndef AIRY_AGENT_D_SOCKET
#define AIRY_AGENT_D_SOCKET airy_runtime_dir_socket("agent.sock")
#endif
#ifndef AIRY_SCHED_D_SOCKET
#define AIRY_SCHED_D_SOCKET airy_runtime_dir_socket("sched.sock")
#endif
#define AIRY_DAEMON_RPC_TIMEOUT_MS 30000

#define RUNTIME_LOCK() airy_mtx_lock(&g_runtime.mutex)
#define RUNTIME_UNLOCK() airy_mtx_unlock(&g_runtime.mutex)

#define MAX_SESSIONS_DEFAULT 64

#define MAX_INPUT_SIZE 4096

typedef struct {
    char *key;
    size_t index;
    bool occupied;
    bool deleted; /**< tombstone：删除标记。P0: 删除槽位不直接置空，
                       否则开放寻址探测链断裂，后续 ht_lookup 会漏查元素 */
} hash_entry_t;

typedef struct {
    hash_entry_t *entries;
    size_t capacity;
    size_t count;
} hash_table_t;

typedef struct {
    char *session_id;
    char *metadata;
    time_t created_at;
    time_t last_accessed;
} session_entry_t;

struct syscall_runtime_s {
    session_entry_t *sessions;
    size_t session_count;
    hash_table_t session_index;
    /* Telemetry fields: real memory/agent counts are managed by mem_d/agent_d
      * (Phase 3); the gateway keeps them at 0. */
    uint64_t record_count;
    uint64_t agent_count;
    uint64_t total_memory_writes;
    time_t start_time; /**< 进程启动时间戳（uptime 计算基准） */
    airy_mtx_t mutex;
    bool initialized;
};

/* Runtime state shared across files (was static; now external linkage) **/
extern size_t g_max_sessions;
extern struct syscall_runtime_s g_runtime;

/* Helpers shared across files (was static; now external linkage) **/
unsigned long hash_fn(const char *str);
int ht_init(hash_table_t *ht, size_t capacity);
void ht_destroy(hash_table_t *ht);
bool ht_insert(hash_table_t *ht, const char *key, size_t index);
ssize_t ht_lookup(hash_table_t *ht, const char *key);
const char *generate_uuid(void);

/* Per-domain route functions (was static; now external linkage) **/
char *route_task_methods(const char *method, cJSON *params, cJSON *request_id);
char *route_memory_methods(const char *method, cJSON *params, cJSON *request_id);
char *route_session_methods(const char *method, cJSON *params, cJSON *request_id);
char *route_telemetry_methods(const char *method, cJSON *params, cJSON *request_id);
char *route_agent_methods(const char *method, cJSON *params, cJSON *request_id);

/* 统一经 syscall 派发（架构约束 2026-08-25）：airy_sys_svc_call + 解包 result */
int syscall_svc_call_unwrap(const char *ns, const char *method, const char *params_json,
                            int timeout_ms, char **out_result);

/* Mem domain thin IPC forwarders (syscall_router_memory.c). Declared here so
 * gateway route handlers (e.g. SSE chat memory injection) can call them with
 * a real prototype instead of an implicit int-returning declaration. */
airy_err_t airy_sys_memory_write(const void *data, size_t len, const char *metadata,
                                 char **out_record_id);
airy_err_t airy_sys_memory_search(const char *query, uint32_t limit, char ***record_ids,
                                  float **scores, size_t *count);
airy_err_t airy_sys_memory_get(const char *record_id, void **out_data, size_t *out_len);
airy_err_t airy_sys_memory_delete(const char *record_id);

#endif /* GATEWAY_SYSCALL_ROUTER_INTERNAL_H */
