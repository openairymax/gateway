/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file ws_gateway.h
 * @brief WebSocket gateway interface.
 */

/* @owner: team-B */
#ifndef AIRY_RT_GATEWAY_WS_H
#define AIRY_RT_GATEWAY_WS_H

#include "gateway_internal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a WebSocket gateway
 *
 * @param host Listen address
 * @param port Listen port
 * @return Gateway instance, or NULL on failure
 *
 * @ownership Caller must release via gateway_destroy()
 */
gateway_t *ws_gateway_create(const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_WS_H */
