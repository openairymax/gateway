// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file syscall_router_runtime.c
 * @brief Syscall router runtime domain (global state, open-addressing hash table, ctor/dtor init).
 */

// @owner: team-B
#include "syscall_router.h"
#include "syscall_router_internal.h"

size_t g_max_tasks = 0;
size_t g_max_sessions = 0;

unsigned long hash_fn(const char *str)
{
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        h = ((h << 5) + h) + c;
    return h;
}

int ht_init(hash_table_t *ht, size_t capacity)
{
    ht->entries = (hash_entry_t *)AIRY_CALLOC(capacity, sizeof(hash_entry_t));
    if (!ht->entries) {
        ht->capacity = 0;
        airy_err_push_ex(AIRY_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                         "ht_init: allocation failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    ht->capacity = capacity;
    ht->count = 0;
    return 0;
}

void ht_destroy(hash_table_t *ht)
{
    if (!ht->entries)
        return;
    for (size_t i = 0; i < ht->capacity; i++) {
        AIRY_FREE(ht->entries[i].key);
    }
    AIRY_FREE(ht->entries);
    ht->entries = NULL;
    ht->capacity = 0;
    ht->count = 0;
}

bool ht_insert(hash_table_t *ht, const char *key, size_t index)
{
    if (!ht->entries || ht->count >= ht->capacity * 3 / 4)
        return false;
    unsigned long h = hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied) {
            /* P0: tombstone slots (deleted=true) are reusable; reset deleted before reuse,
              * or ht_lookup skips them */
            ht->entries[pos].key = AIRY_STRDUP(key);
            ht->entries[pos].index = index;
            ht->entries[pos].occupied = true;
            ht->entries[pos].deleted = false;
            ht->count++;
            return true;
        }
    }
    return false;
}

ssize_t ht_lookup(hash_table_t *ht, const char *key)
{
    if (!ht->entries || ht->count == 0) {
        airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "ht_lookup: failed");
        return AIRY_ERR_UNKNOWN;
    }
    unsigned long h = hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied) {
            if (ht->entries[pos].deleted)
                continue;
            airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "hash_fn: failed");
            return AIRY_ERR_UNKNOWN;
        }
        if (strcmp(ht->entries[pos].key, key) == 0)
            return (ssize_t)ht->entries[pos].index;
    }
    airy_err_push_ex(AIRY_ERR_UNKNOWN, __FILE__, __LINE__, __func__, "if: failed");
    return AIRY_ERR_UNKNOWN;
}

struct syscall_runtime_s g_runtime = {0};

static void __attribute__((constructor)) runtime_init(void)
{
    airy_mtx_init(&g_runtime.mutex);

    const char *env;
    g_max_tasks = MAX_TASKS_DEFAULT;
    g_max_sessions = MAX_SESSIONS_DEFAULT;

    env = getenv("AIRY_MAX_TASKS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_max_tasks = (size_t)v;
    }
    env = getenv("AIRY_MAX_SESSIONS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_max_sessions = (size_t)v;
    }
    /* Phase 3: memory/agent capacity is managed by the mem_d/agent_d daemons;
      * AIRY_MAX_RECORDS / AIRY_MAX_AGENTS env vars are forwarded to the daemons. */

    g_runtime.tasks = (task_entry_t *)AIRY_CALLOC(g_max_tasks, sizeof(task_entry_t));
    g_runtime.sessions = (session_entry_t *)AIRY_CALLOC(g_max_sessions, sizeof(session_entry_t));
    if (!g_runtime.tasks || !g_runtime.sessions) {
        AIRY_LOG_ERROR("syscall_router: runtime_init calloc failed");
        AIRY_FREE(g_runtime.tasks);
        AIRY_FREE(g_runtime.sessions);
        g_runtime.tasks = NULL;
        g_runtime.sessions = NULL;
        return;
    }
    if (ht_init(&g_runtime.task_index, g_max_tasks * 2) != 0 ||
        ht_init(&g_runtime.session_index, g_max_sessions * 2) != 0) {
        ht_destroy(&g_runtime.task_index);
        ht_destroy(&g_runtime.session_index);
        AIRY_FREE(g_runtime.tasks);
        AIRY_FREE(g_runtime.sessions);
        g_runtime.tasks = NULL;
        g_runtime.sessions = NULL;
        return;
    }
    g_runtime.initialized = true;
}

static void __attribute__((destructor)) runtime_cleanup(void)
{

    for (size_t i = 0; i < g_runtime.task_count; i++) {
        AIRY_FREE(g_runtime.tasks[i].task_id);
        AIRY_FREE(g_runtime.tasks[i].input);
        AIRY_FREE(g_runtime.tasks[i].result);
    }

    for (size_t i = 0; i < g_runtime.session_count; i++) {
        AIRY_FREE(g_runtime.sessions[i].session_id);
        AIRY_FREE(g_runtime.sessions[i].metadata);
    }

    airy_mtx_destroy(&g_runtime.mutex);
    ht_destroy(&g_runtime.task_index);
    ht_destroy(&g_runtime.session_index);
    AIRY_FREE(g_runtime.tasks);
    AIRY_FREE(g_runtime.sessions);
    g_runtime.tasks = NULL;
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
