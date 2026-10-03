// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_runtime.c
 * @brief Syscall router runtime domain (global state, ctor/dtor init).
 *
 * 会话 id→下标索引委托 commons/utils/ds/hindex（唯一机制源）；本文件不再
 * 自带开放寻址哈希实现。
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

size_t g_max_sessions = 0;

struct syscall_runtime_s g_runtime = {0};

static void __attribute__((constructor)) runtime_init(void)
{
    airy_mtx_init(&g_runtime.mutex);
    g_runtime.start_time = time(NULL);

    g_max_sessions = MAX_SESSIONS_DEFAULT;

    const char *env = getenv("AIRY_MAX_SESSIONS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_max_sessions = (size_t)v;
    }
    /* Phase 3: memory/agent/task capacity is managed by the mem_d/agent_d/sched_d
      * daemons; AIRY_MAX_RECORDS / AIRY_MAX_AGENTS env vars are forwarded to the
      * daemons. */

    g_runtime.sessions = (session_entry_t *)AIRY_CALLOC(g_max_sessions, sizeof(session_entry_t));
    if (!g_runtime.sessions) {
        AIRY_LOG_ERROR("syscall_router: runtime_init calloc failed");
        g_runtime.sessions = NULL;
        return;
    }
    if (hindex_init(&g_runtime.session_index, g_max_sessions * 2) != AIRY_SUCCESS) {
        hindex_free(&g_runtime.session_index);
        AIRY_FREE(g_runtime.sessions);
        g_runtime.sessions = NULL;
        return;
    }
    g_runtime.initialized = true;
}

static void __attribute__((destructor)) runtime_cleanup(void)
{

    for (size_t i = 0; i < g_runtime.session_count; i++) {
        AIRY_FREE(g_runtime.sessions[i].session_id);
        AIRY_FREE(g_runtime.sessions[i].metadata);
    }

    airy_mtx_destroy(&g_runtime.mutex);
    hindex_free(&g_runtime.session_index);
    AIRY_FREE(g_runtime.sessions);
    g_runtime.sessions = NULL;
    g_runtime.initialized = false;
}

const char *generate_uuid(void)
{
    static char uuid[37];
    static uint64_t counter = 0;
    snprintf(uuid, sizeof(uuid), "agentrt-%016llx-%08llx", (unsigned long long)time(NULL),
             (unsigned long long)++counter);
    return uuid;
}
