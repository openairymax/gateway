/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
/**
 * @file gateway_protocol_handler_internal.h
 * @brief Multi-protocol gateway request handler - internal shared declarations.
 *
 * After gateway_protocol_handler.c was split by functional domain, this
 * header carries the cross-file helper declarations:
 *   - gateway_protocol_handler.c  handler lifecycle / main handling / stats
 *   - gateway_protocol_convert.c  protocol-to-JSON-RPC conversion
 * Body protocol detection lives in the protocols mechanism core
 * (protocol_detect.h, proto_detect_body).
 */

#ifndef AIRY_RT_GATEWAY_PROTOCOL_HANDLER_INTERNAL_H
#define AIRY_RT_GATEWAY_PROTOCOL_HANDLER_INTERNAL_H

#include "gateway_protocol_handler.h"

#include "airy_protocol_interface.h"

#include "error.h"
#include "airy_memory.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Conversion domain (gateway_protocol_convert.c) */
rpc_result_t create_error_result(int code, const char *message, const char *id_str);

cJSON *gw_llm_to_rpc(const char *request_data, size_t request_size,
                     char **out_method, char **out_id);
cJSON *gw_mcp_to_rpc(const char *request_data, size_t request_size,
                     char **out_method, char **out_id);
cJSON *gw_a2a_to_rpc(const char *request_data, size_t request_size,
                     char **out_method, char **out_id);

char *gw_proto_resp(airy_protocol_type_t type, const cJSON *result);
char *gw_req_text(const char *method, const char *id, cJSON *params);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_PROTOCOL_HANDLER_INTERNAL_H */
