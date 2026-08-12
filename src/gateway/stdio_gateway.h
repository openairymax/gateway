/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file stdio_gateway.h
 * @brief Stdio gateway interface.
 */

/* @owner: team-B */
#ifndef AIRY_RT_GATEWAY_STDIO_H
#define AIRY_RT_GATEWAY_STDIO_H

#include "gateway_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a stdio gateway
 *
 * @return Gateway instance, or NULL on failure
 *
 * @ownership Caller must release via gateway_destroy()
 */
gateway_t *stdio_gateway_create(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_STDIO_H */
