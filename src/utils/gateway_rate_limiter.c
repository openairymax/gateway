// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_rate_limiter.c
 * @brief Built-in gateway rate limiter implementation.
 *
 * Implements token-bucket based rate limiting.
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
  * @brief Per-client rate limit state
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
  * @brief Simple string hash function (djb2)
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
  * @brief Create a client state
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
  * @brief Destroy a client state
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
  * @brief Find or create a client state
 *
  * @note P0: caller must hold limiter->table_lock. Mutually exclusive with
  * cleanup_expired_clients so a client is not freed while check_rate_limit reads it (UAF).
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
  * @brief Update the token count
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
  * @brief Check the time-window counter
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
  * @brief Clean up long-inactive clients
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
  * @brief Precondition checks
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
  * @brief Possibly run cleanup (time-interval based)
 */
static inline void maybe_cleanup_clients(gateway_rate_limiter_t *limiter, time_t now)
{
    if (now - limiter->last_cleanup_time >= limiter->config.cleanup_interval_sec) {
        cleanup_expired_clients(limiter);
    }
}

/**
  * @brief Consume a token and update counters
 */
static inline void consume_token(client_state_t *client)
{
    client->tokens--;
    client->request_count_minute++;
    client->request_count_hour++;
}

/**
  * @brief Check the rate limit (core logic)
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

    /* Steps 4-5: P0 - client lookup and state read/write must happen under the
      * same lock as cleanup_expired_clients. Otherwise the cleaner may free the
      * client mid-read/write (data race + UAF). */
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
        /* On allocation failure, allow the request (availability first) so memory pressure
          * does not take the whole gateway down. Known trade-off: limits may be bypassed under OOM. */
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
