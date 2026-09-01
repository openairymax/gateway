/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_tools_schema.h
 * @brief Gateway OpenAI tools schema re-export 兼容头（IRON-6 模式，M1-1a）。
 *
 * 工具目录 OpenAI tools schema 的单一权威已下沉 commons 契约层
 * （commons/include/airy_tool_schema.h，AIRY_TOOLS_JSON_SOURCE）。
 * 本头保留 GW_TOOLS_JSON_SOURCE 旧名以便存量引用平滑迁移，禁止新增
 * 使用方；新增引用一律直接包含 "airy_tool_schema.h"。
 */

/* @owner: team-B */
#ifndef GATEWAY_TOOLS_SCHEMA_H
#define GATEWAY_TOOLS_SCHEMA_H

#include "airy_tool_schema.h"

#define GW_TOOLS_JSON_SOURCE AIRY_TOOLS_JSON_SOURCE

#endif /* GATEWAY_TOOLS_SCHEMA_H */
