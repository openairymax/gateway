// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * @file gateway_rate_limiter.c
 * @brief Gateway 内置速率限制器实现
 *
 * 实现基于令牌桶算法的速率限制功能。
 *
 */

// @owner: team-B
#include "atomic_compat.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define strdup _strdup
#endif

#include "error.h"
#include "gateway_rate_limiter.h"
#include "gateway_utils.h"
#include "airy_memory.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief 单个客户端的速率限制状态
 */
typedef struct client_state {
    char *client_key;
    uint64_t tokens;
    uint64_t last_update_ns;
    uint32_t request_count_minute;
    uint32_t request_count_hour;
    uint64_t minute_start_ns;
    uint64_t hour_start_ns;
    time_t last_access_time;
    struct client_state *next;
} client_state_t;

struct gateway_rate_limiter {
    gateway_rate_limit_config_t config;

    client_state_t **clients_table;
    size_t table_size;

    atomic_uint_fast64_t total_allowed;
    atomic_uint_fast64_t total_rejected;
    atomic_uint_fast32_t active_clients;

    atomic_bool running;
    time_t last_cleanup_time;

#ifdef _WIN32
    airy_mtx_t table_lock;
#else
    airy_mtx_t table_lock;
#endif
};

/**
 * @brief 简单的字符串哈希函数（djb2算法）
 */
static uint32_t hash_string(const char *str, size_t table_size)
{
    uint32_t hash = 5381;
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }

    return hash % table_size;
}

/**
 * @brief 创建客户端状态
 */
static client_state_t *client_state_create(const char *client_key, uint64_t now_ns)
{
    client_state_t *state = AIRY_CALLOC(1, sizeof(client_state_t));
    if (!state)
        return NULL;

    state->client_key = AIRY_STRDUP(client_key);
    if (!state->client_key) {
        AIRY_FREE(state);
        return NULL;
    }

    state->tokens = 0;
    state->last_update_ns = now_ns;
    state->request_count_minute = 0;
    state->request_count_hour = 0;
    state->minute_start_ns = now_ns;
    state->hour_start_ns = now_ns;
    state->last_access_time = time(NULL);
    state->next = NULL;

    return state;
}

/**
 * @brief 销毁客户端状态
 */
static void client_state_destroy(client_state_t *state)
{
    if (!state)
        return;

    if (state->client_key) {
        AIRY_FREE(state->client_key);
    }
    AIRY_FREE(state);
}

/**
 * @brief 查找或创建客户端状态
 *
 * @note P0: 调用者必须持有 limiter->table_lock。与 cleanup_expired_clients
 * 互斥，确保 client 在 check_rate_limit 读写期间不会被并发 free（UAF）。
 */
static client_state_t *get_or_create_client_locked(gateway_rate_limiter_t *limiter,
                                                   const char *client_key, uint64_t now_ns)
{
    uint32_t hash = hash_string(client_key, limiter->table_size);

    client_state_t *current = limiter->clients_table[hash];
    while (current) {
        if (strcmp(current->client_key, client_key) == 0) {
            current->last_access_time = time(NULL);
            return current;
        }
        current = current->next;
    }

    client_state_t *new_client = client_state_create(client_key, now_ns);
    if (!new_client)
        return NULL;

    new_client->next = limiter->clients_table[hash];
    limiter->clients_table[hash] = new_client;

    atomic_fetch_add(&limiter->active_clients, 1);

    return new_client;
}

/**
 * @brief 更新令牌数
 */
static void update_tokens(client_state_t *client, const gateway_rate_limit_config_t *config,
                          uint64_t now_ns)
{
    if (now_ns <= client->last_update_ns)
        return;

    uint64_t elapsed_ns = now_ns - client->last_update_ns;

    uint64_t new_tokens = (elapsed_ns * config->requests_per_second) / 1000000000ULL;

    client->tokens += new_tokens;
    if (client->tokens > config->burst_size) {
        client->tokens = config->burst_size;
    }

    client->last_update_ns = now_ns;
}

/**
 * @brief 检查时间窗口计数器
 */
static bool check_time_windows(client_state_t *client, const gateway_rate_limit_config_t *config,
                               uint64_t now_ns)
{

    uint64_t minute_elapsed = now_ns - client->minute_start_ns;
    if (minute_elapsed >= 60000000000ULL) {
        client->request_count_minute = 0;
        client->minute_start_ns = now_ns;
    }

    if (config->requests_per_minute > 0 &&
        client->request_count_minute >= config->requests_per_minute) {
        return false;
    }

    uint64_t hour_elapsed = now_ns - client->hour_start_ns;
    if (hour_elapsed >= 3600000000000ULL) {
        client->request_count_hour = 0;
        client->hour_start_ns = now_ns;
    }

    if (config->requests_per_hour > 0 && client->request_count_hour >= config->requests_per_hour) {
        return false;
    }

    return true;
}

/**
 * @brief 清理长时间未活跃的客户端
 */
static void cleanup_expired_clients(gateway_rate_limiter_t *limiter)
{
    time_t now = time(NULL);
    time_t expire_threshold = now - 3600;

#ifdef _WIN32
    airy_mtx_lock(&limiter->table_lock);
#else
    airy_mtx_lock(&limiter->table_lock);
#endif

    for (size_t i = 0; i < limiter->table_size; i++) {
        client_state_t **current = &limiter->clients_table[i];

        while (*current) {
            if ((*current)->last_access_time < expire_threshold) {
                client_state_t *to_delete = *current;
                *current = (*current)->next;
                client_state_destroy(to_delete);
                atomic_fetch_sub(&limiter->active_clients, 1);
            } else {
                current = &(*current)->next;
            }
        }
    }

    limiter->last_cleanup_time = now;

#ifdef _WIN32
    airy_mtx_unlock(&limiter->table_lock);
#else
    airy_mtx_unlock(&limiter->table_lock);
#endif
}

/**
 * @brief 前置条件检查
 */
static inline bool is_rate_limiter_valid(const gateway_rate_limiter_t *limiter,
                                         const char *client_key)
{
    if (!limiter || !client_key)
        return false;
    if (!limiter->config.enabled)
        return true;
    return true;
}

/**
 * @brief 可能执行清理（基于时间间隔）
 */
static inline void maybe_cleanup_clients(gateway_rate_limiter_t *limiter, time_t now)
{
    if (now - limiter->last_cleanup_time >= limiter->config.cleanup_interval_sec) {
        cleanup_expired_clients(limiter);
    }
}

/**
 * @brief 消耗令牌并更新计数器
 */
static inline void consume_token(client_state_t *client)
{
    client->tokens--;
    client->request_count_minute++;
    client->request_count_hour++;
}

/**
 * @brief 检查速率限制（核心逻辑）
 */
static bool check_rate_limit(client_state_t *client, gateway_rate_limiter_t *limiter,
                             const gateway_rate_limit_config_t *config, uint64_t now_ns)
{

    update_tokens(client, config, now_ns);

    if (!check_time_windows(client, config, now_ns)) {
        if (limiter) {
            atomic_fetch_add(&limiter->total_rejected, 1);
        }
        return false;
    }

    if (client->tokens > 0) {
        consume_token(client);
        if (limiter) {
            atomic_fetch_add(&limiter->total_allowed, 1);
        }
        return true;
    }

    if (limiter) {
        atomic_fetch_add(&limiter->total_rejected, 1);
    }
    return false;
}

void gateway_rate_limiter_get_default_config(gateway_rate_limit_config_t *config)
{
    if (!config)
        return;

    config->enabled = false;
    config->requests_per_second = 100;
    config->requests_per_minute = 6000;
    config->requests_per_hour = 360000;
    config->burst_size = 150;
    config->cleanup_interval_sec = 300;
}

gateway_rate_limiter_t *gateway_rate_limiter_create(const gateway_rate_limit_config_t *config)
{
    gateway_rate_limiter_t *limiter = AIRY_CALLOC(1, sizeof(gateway_rate_limiter_t));
    if (!limiter)
        return NULL;

    if (config) {
        limiter->config = *config;
    } else {
        gateway_rate_limiter_get_default_config(&limiter->config);
    }

    const char *env_ts = getenv("AIRY_RATE_LIMIT_TABLE_SIZE");
    if (env_ts) {
        unsigned long v = strtoul(env_ts, NULL, 10);
        limiter->table_size = (v > 0 && v < 100000) ? (size_t)v : 1021;
    } else {
        limiter->table_size = 1021;
    }
    limiter->clients_table = AIRY_CALLOC(limiter->table_size, sizeof(client_state_t *));
    if (!limiter->clients_table) {
        AIRY_FREE(limiter);
        return NULL;
    }

    atomic_init(&limiter->total_allowed, 0);
    atomic_init(&limiter->total_rejected, 0);
    atomic_init(&limiter->active_clients, 0);
    atomic_init(&limiter->running, true);

#ifdef _WIN32
    airy_mtx_init(&limiter->table_lock);
#else
    airy_mtx_init(&limiter->table_lock);
#endif

    limiter->last_cleanup_time = time(NULL);

    return limiter;
}

void gateway_rate_limiter_destroy(gateway_rate_limiter_t *limiter)
{
    if (!limiter)
        return;

    atomic_store(&limiter->running, false);

#ifdef _WIN32
    airy_mtx_lock(&limiter->table_lock);
#else
    airy_mtx_lock(&limiter->table_lock);
#endif

    if (limiter->clients_table) {
        for (size_t i = 0; i < limiter->table_size; i++) {
            client_state_t *current = limiter->clients_table[i];
            while (current) {
                client_state_t *next = current->next;
                client_state_destroy(current);
                current = next;
            }
        }
        AIRY_FREE(limiter->clients_table);
    }

#ifdef _WIN32
    airy_mtx_unlock(&limiter->table_lock);
    airy_mtx_destroy(&limiter->table_lock);
#else
    airy_mtx_unlock(&limiter->table_lock);
    airy_mtx_destroy(&limiter->table_lock);
#endif

    AIRY_FREE(limiter);
}

bool gateway_rate_limiter_allow(gateway_rate_limiter_t *limiter, const char *client_key)
{

    if (!is_rate_limiter_valid(limiter, client_key)) {
        return true;
    }

    uint64_t now_ns = gateway_time_ns();
    time_t now = time(NULL);

    maybe_cleanup_clients(limiter, now);

    /* 步骤 4-5: P0 — client 的获取与状态读写必须与 cleanup_expired_clients
     * 在同一把锁下完成。否则清理线程可能在 check_rate_limit 读写 client
     * 期间将其 free（数据竞争 + UAF）。 */
#ifdef _WIN32
    airy_mtx_lock(&limiter->table_lock);
#else
    airy_mtx_lock(&limiter->table_lock);
#endif

    client_state_t *client = get_or_create_client_locked(limiter, client_key, now_ns);
    if (!client) {
#ifdef _WIN32
        airy_mtx_unlock(&limiter->table_lock);
#else
        airy_mtx_unlock(&limiter->table_lock);
#endif
        /* 内存分配失败：采用可用性优先策略放行，避免内存压力导致网关全量拒绝服务。
         * 此为 fail-open 取舍——已知风险：极端 OOM 下速率限制可被绕过。 */
        return true;
    }

    bool allowed = check_rate_limit(client, limiter, &limiter->config, now_ns);

#ifdef _WIN32
    airy_mtx_unlock(&limiter->table_lock);
#else
    airy_mtx_unlock(&limiter->table_lock);
#endif
    return allowed;
}

void gateway_rate_limiter_get_stats(const gateway_rate_limiter_t *limiter, uint64_t *total_allowed,
                                    uint64_t *total_rejected, uint32_t *active_clients)
{
    if (!limiter)
        return;

    if (total_allowed) {
        *total_allowed = atomic_load(&limiter->total_allowed);
    }
    if (total_rejected) {
        *total_rejected = atomic_load(&limiter->total_rejected);
    }
    if (active_clients) {
        *active_clients = atomic_load(&limiter->active_clients);
    }
}

void gateway_rate_limiter_reset_client(gateway_rate_limiter_t *limiter, const char *client_key)
{
    if (!limiter || !client_key)
        return;

    uint32_t hash = hash_string(client_key, limiter->table_size);

#ifdef _WIN32
    airy_mtx_lock(&limiter->table_lock);
#else
    airy_mtx_lock(&limiter->table_lock);
#endif

    client_state_t *current = limiter->clients_table[hash];
    while (current) {
        if (strcmp(current->client_key, client_key) == 0) {
            current->tokens = 0;
            current->request_count_minute = 0;
            current->request_count_hour = 0;
            current->last_update_ns = gateway_time_ns();

#ifdef _WIN32
            airy_mtx_unlock(&limiter->table_lock);
#else
            airy_mtx_unlock(&limiter->table_lock);
#endif
            return;
        }
        current = current->next;
    }

#ifdef _WIN32
    airy_mtx_unlock(&limiter->table_lock);
#else
    airy_mtx_unlock(&limiter->table_lock);
#endif
}
