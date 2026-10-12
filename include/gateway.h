/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file gateway.h
 * @brief AgentRT gateway unified public interface.
 *
 * The gateway layer only performs protocol translation, converting external
 * requests into agentrt/atoms/syscall calls.
 *
 * Architecture: agentrt/daemons/gateway_d/ -> agentrt/gateway/ -> atoms/syscall/
 *
 * Design principles (per ARCHITECTURAL_PRINCIPLES.md):
 *   K-1 minimal core: the gateway only translates protocols, zero business logic
 *   K-2 contract interfaces: all APIs carry full Doxygen contracts
 *   S-2 layered decomposition: strict layering, no cross-layer access
 */

/* @owner: team-B */
#ifndef AIRY_RT_GATEWAY_H
#define AIRY_RT_GATEWAY_H

#include "airy_rt.h"

#include "airy_abi.h"

AIRY_ABI_BEGIN

/**
  * @brief Gateway-specific error codes (extend the AgentRT standard error codes)
 *
 * @note Gateway APIs return both airy_err_t and gateway_error_t; the latter
 *       covers gateway-specific error scenarios.
 */
typedef enum {
    GATEWAY_SUCCESS = 0,
    GATEWAY_ERROR_INVALID = -1,
    GATEWAY_ERROR_MEMORY = -2,
    GATEWAY_ERROR_IO = -3,
    GATEWAY_ERROR_TIMEOUT = -4,
    GATEWAY_ERROR_CLOSED = -5,
    GATEWAY_ERROR_PROTOCOL = -6
} gateway_error_t;


/**
  * @brief Gateway type enumeration
 */
typedef enum { GATEWAY_TYPE_HTTP = 0, GATEWAY_TYPE_WS, GATEWAY_TYPE_STDIO } gateway_type_t;


/**
  * @brief Opaque gateway handle
 *
 * @note Internally holds an ops table + impl pointer + type tag.
 */
typedef struct gateway gateway_t;

/**
  * @brief Destroy the gateway instance and free all resources
 *
 * Stops a running gateway automatically, then frees all associated resources.
 * Silently ignores NULL input.
 *
 * @param[in] gw Gateway handle (may be NULL, silently ignored)
 *
 * @ownership Transfers ownership of the handle; gw must not be used afterwards
 * @threadsafe no, the caller must serialize
 * @since 1.0.0
 */
void gateway_destroy(gateway_t *gw);


/**
  * @brief Start the gateway
 *
 * Start listening/accepting input. HTTP/WS gateways start non-blocking
 * (background thread); the stdio gateway starts blocking (REPL loop).
 *
 * @param[in] gw Gateway handle
 * @return AIRY_SUCCESS on success
 * @return AIRY_EINVAL invalid parameters
 * @return AIRY_EBUSY port busy or resource busy
 *
 * @pre The gateway was created by a transport constructor
 *      (http/ws/stdio/http2_gateway_create)
 * @post The gateway is running and accepting requests
 * @threadsafe yes
 * @since 1.0.0
 */
int gateway_start(gateway_t *gw);

AIRY_ABI_END

#endif /* AIRY_RT_GATEWAY_H */
