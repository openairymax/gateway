/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file gateway_rate_limiter.h
 * @brief Built-in gateway rate limiter.
 *
 * Token-bucket rate limiting to prevent DoS attacks and resource abuse,
 * keyed by client IP or API key.
 */

/* @owner: team-B */
#ifndef AIRY_RT_GATEWAY_RATE_LIMITER_H
#define AIRY_RT_GATEWAY_RATE_LIMITER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief Rate limiter configuration
 */
typedef struct {
    bool enabled;
    uint32_t requests_per_second;
    uint32_t requests_per_minute;
    uint32_t requests_per_hour;
    uint32_t burst_size;
    uint32_t cleanup_interval_sec;
} gateway_rate_limit_config_t;

/**
  * @brief Rate limiter instance (opaque pointer)
 */
typedef struct gateway_rate_limiter gateway_rate_limiter_t;

/**
  * @brief Create a rate limiter
 *
  * @param config Configuration parameters
  * @return Rate limiter instance, or NULL on failure
 *
 * @ownership Caller must release via gateway_rate_limiter_destroy()
 * @threadsafe yes
 * @since 1.0.0
 */
gateway_rate_limiter_t *gateway_rate_limiter_create(const gateway_rate_limit_config_t *config);

/**
  * @brief Destroy a rate limiter
 *
 * @param limiter Rate limiter instance
 *
 * @threadsafe yes
 * @since 1.0.0
 */
void gateway_rate_limiter_destroy(gateway_rate_limiter_t *limiter);

/**
  * @brief Check whether a request is allowed
 *
 * @param limiter Rate limiter instance
  * @param client_key Client identifier (IP address or API key)
  * @return true if allowed, false if rejected
 *
 * @threadsafe yes
 * @since 1.0.0
 */
bool gateway_rate_limiter_allow(gateway_rate_limiter_t *limiter, const char *client_key);

/**
  * @brief Get the current limit state (for monitoring)
 *
 * @param limiter Rate limiter instance
  * @param total_allowed Output: total allowed requests
  * @param total_rejected Output: total rejected requests
  * @param active_clients Output: active client count
 *
 * @threadsafe yes
 * @since 1.0.0
 */
void gateway_rate_limiter_get_stats(const gateway_rate_limiter_t *limiter, uint64_t *total_allowed,
                                    uint64_t *total_rejected, uint32_t *active_clients);

/**
  * @brief Reset the counters of a given client
 *
 * @param limiter Rate limiter instance
  * @param client_key Client identifier
 *
 * @threadsafe yes
 * @since 1.0.0
 */
void gateway_rate_limiter_reset_client(gateway_rate_limiter_t *limiter, const char *client_key);

/**
  * @brief Get the default configuration
 *
  * @param config Output configuration structure
 *
 * @since 1.0.0
 */
void gateway_rate_limiter_get_default_config(gateway_rate_limit_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_RATE_LIMITER_H */
