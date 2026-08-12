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
#ifndef AIRY_MEM_D_SOCKET
#define AIRY_MEM_D_SOCKET AIRY_RUNTIME_DIR "/mem.sock"
#endif
#ifndef AIRY_AGENT_D_SOCKET
#define AIRY_AGENT_D_SOCKET AIRY_RUNTIME_DIR "/agent.sock"
#endif
#define AIRY_DAEMON_RPC_TIMEOUT_MS 30000

#define RUNTIME_LOCK() airy_mtx_lock(&g_runtime.mutex)
#define RUNTIME_UNLOCK() airy_mtx_unlock(&g_runtime.mutex)

#define MAX_TASKS_DEFAULT 256
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
    char *task_id;
    char *input;
    size_t input_len;
    int status;
    char *result;
    uint32_t timeout_ms;
    time_t created_at;
} task_entry_t;

typedef struct {
    char *session_id;
    char *metadata;
    time_t created_at;
    time_t last_accessed;
} session_entry_t;

struct syscall_runtime_s {
    task_entry_t *tasks;
    size_t task_count;
    hash_table_t task_index;
    session_entry_t *sessions;
    size_t session_count;
    hash_table_t session_index;
    uint64_t total_tasks_submitted;
    /* Telemetry fields: real memory/agent counts are managed by mem_d/agent_d
      * (Phase 3); the gateway keeps them at 0. */
    uint64_t record_count;
    uint64_t agent_count;
    uint64_t total_memory_writes;
    airy_mtx_t mutex;
    bool initialized;
};

/* Runtime state shared across files (was static; now external linkage) **/
extern size_t g_max_tasks;
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

#endif /* GATEWAY_SYSCALL_ROUTER_INTERNAL_H */
