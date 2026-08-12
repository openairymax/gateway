/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file gateway_utils.h
 * @brief Common utility functions for the gateway module.
 *
 * Provides helpers shared by all gateway implementations to avoid code
 * duplication and definition conflicts.
 */

/* @owner: team-B */
#ifndef GATEWAY_UTILS_H
#define GATEWAY_UTILS_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief Get the current monotonic clock time (nanosecond precision)
 *
  * Cross-platform high-resolution time function。
  * Windows uses FILETIME, POSIX uses clock_gettime。
 *
  * @return Current timestamp (ns), based on CLOCK_REALTIME/system boot time
 *
 * @threadsafe yes (read-only operation)
  * @note Precision: ~100ns on Windows, ~1ns on POSIX (hardware dependent)
 */
static inline uint64_t gateway_time_ns(void)
{
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;


    return (uli.QuadPart - 116444736000000000ULL) * 100;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/**
  * @brief Cross-platform sleep function
 *
  * @param seconds Seconds to sleep
 */
static inline void gateway_sleep(unsigned int seconds)
{
#ifdef _WIN32
    Sleep(seconds * 1000);
#else
    sleep(seconds);
#endif
}

/**
  * @brief Compute elapsed time (nanoseconds)
 *
  * @param start_ns Start timestamp
  * @return Nanoseconds elapsed from start_ns to now
 */
static inline uint64_t gateway_elapsed_ns(uint64_t start_ns)
{
    return gateway_time_ns() - start_ns;
}

/**
  * @brief Convert nanoseconds to milliseconds
 *
 * @param ns Nanoseconds value
  * @return Millisecond value
 */
static inline uint64_t gateway_ns_to_ms(uint64_t ns)
{
    return ns / 1000000ULL;
}

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_UTILS_H */
