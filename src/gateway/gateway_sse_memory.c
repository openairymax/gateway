// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_sse_memory.c
 * @brief Long-term memory injection and write-back for SSE chat sessions.
 *
 * Extracted from http_gateway_sse.c — owns the mem_d integration: semantic
 * search + inject relevant history as a system prefix message, and record
 * the completed "user:/AgentRT:" pair after the session ends (aligned with
 * the CLI cli_chat_mem_inject_system / cli_chat_mem_record path so that
 * CLI and TUI share the same recall corpus).
 */

#include "http_gateway_sse_internal.h"

/* ── UTF-8 sanitiser ───────────────────────────────────────────────── */

char *gw_sse_utf8_sanitize(const char *s, size_t len)
{
    if (!s)
        return NULL;
    char *out = (char *)AIRY_MALLOC(len * 3 + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need = 0;
        if (c < 0x80) {
            out[o++] = (char)c;
            i += 1;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
        }
        int valid = 1;
        for (size_t k = 1; need && k < need; ++k) {
            if (i + k >= len || ((unsigned char)s[i + k] & 0xC0) != 0x80) {
                valid = 0;
                break;
            }
        }
        if (need && valid) {
            for (size_t k = 0; k < need; ++k)
                out[o++] = s[i + k];
            i += need;
        } else {
            out[o++] = (char)0xEF;
            out[o++] = (char)0xBF;
            out[o++] = (char)0xBD;
            i += 1;
        }
    }
    out[o] = '\0';
    return out;
}

/* ── Self-feedback filter ──────────────────────────────────────────── */

static int gw_sse_mem_is_self_feedback(const char *record_data, const char *prompt)
{
    if (!record_data || !prompt || !prompt[0])
        return 0;
    if (strncmp(record_data, "用户: ", 9) != 0)
        return 0;
    size_t plen = strlen(prompt);
    if (strncmp(record_data + 9, prompt, plen) == 0)
        return 1;
    return 0;
}

/* ── Memory injection ──────────────────────────────────────────────── */

void gw_sse_mem_inject(cJSON *history, const char *prompt)
{
    if (!history || !prompt || !prompt[0])
        return;

    char **record_ids = NULL;
    float *scores = NULL;
    size_t count = 0;
    if (airy_sys_memory_search(prompt, 3, &record_ids, &scores, &count) != AIRY_OK ||
        count == 0) {
        goto inject_done;
    }

    char mem_acc[768];
    size_t off = 0;
    if (sizeof(mem_acc) > 0) {
        int w0 = snprintf(mem_acc + off, sizeof(mem_acc) - off, "\n\n[相关历史记忆]");
        if (w0 > 0)
            off += ((size_t)w0 < sizeof(mem_acc) - off) ? (size_t)w0 : (sizeof(mem_acc) - off - 1);
    }
    for (size_t i = 0; i < count && off < sizeof(mem_acc) - 1; i++) {
        void *data = NULL;
        size_t dlen = 0;
        if (airy_sys_memory_get(record_ids[i], &data, &dlen) != AIRY_OK || !data)
            continue;
        const char *rec = (const char *)data;
        if (!gw_sse_mem_is_self_feedback(rec, prompt)) {
            size_t n = dlen < 200 ? dlen : 200;
            int w1 = snprintf(mem_acc + off, sizeof(mem_acc) - off, "\n- %.*s", (int)n, rec);
            if (w1 > 0)
                off += ((size_t)w1 < sizeof(mem_acc) - off) ? (size_t)w1
                                                            : (sizeof(mem_acc) - off - 1);
        }
        AIRY_FREE(data);
    }
    if (off > 0 && off < sizeof(mem_acc)) {
        char *clean = gw_sse_utf8_sanitize(mem_acc, off);
        cJSON *sys = cJSON_CreateObject();
        if (sys) {
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", clean ? clean : mem_acc);
            cJSON_InsertItemInArray(history, 0, sys);
        }
        AIRY_FREE(clean);
    }

inject_done:
    if (record_ids) {
        for (size_t i = 0; i < count; i++)
            AIRY_FREE(record_ids[i]);
        AIRY_FREE(record_ids);
    }
    if (scores)
        AIRY_FREE(scores);
}

/* ── JSON string escape (for metadata) ─────────────────────────────── */

static size_t gw_json_escape_append(char *dst, size_t cap, const char *src, size_t len)
{
    if (!dst || !src || cap == 0)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 6 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '"':
            dst[o++] = '\\';
            dst[o++] = '"';
            break;
        case '\\':
            dst[o++] = '\\';
            dst[o++] = '\\';
            break;
        case '\n':
            dst[o++] = '\\';
            dst[o++] = 'n';
            break;
        case '\r':
            dst[o++] = '\\';
            dst[o++] = 'r';
            break;
        case '\t':
            dst[o++] = '\\';
            dst[o++] = 't';
            break;
        default:
            if (c < 0x20) {
                o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
            } else {
                dst[o++] = (char)c;
            }
            break;
        }
    }
    dst[o] = '\0';
    return o;
}

/* ── Memory write-back ─────────────────────────────────────────────── */

void gw_sse_mem_record(gw_sse_ctx_t *sctx)
{
    if (!sctx || sctx->mem_recorded || !sctx->user_prompt || !sctx->user_prompt[0])
        return;
    sctx->mem_recorded = 1;
    if (!sctx->final_text || strlen(sctx->final_text) < 8)
        return;

    char content[1800];
    int n = snprintf(content, sizeof(content), "用户: %s\nAgentRT: %s", sctx->user_prompt,
                     sctx->final_text);
    if (n <= 0)
        return;
    if (n > 1600)
        n = 1600;

    char meta[1024];
    {
        int mn = snprintf(meta, sizeof(meta),
                          "{\"source\":\"gateway\",\"kind\":\"chat\","
                          "\"prompt_tokens\":%llu,\"completion_tokens\":%llu,"
                          "\"total_tokens\":%llu,\"cost_usd\":%.6f",
                          sctx->prompt_tokens, sctx->completion_tokens, sctx->total_tokens,
                          sctx->cost_usd);
        if (sctx->reasoning && sctx->reasoning[0]) {
            size_t rl = strlen(sctx->reasoning);
            if (rl > 600)
                rl = 600;
            if (mn < (int)sizeof(meta) - 2) {
                meta[mn++] = ',';
                meta[mn++] = '"';
                const char *rk = "reasoning\":\"";
                for (const char *p = rk; *p && mn < (int)sizeof(meta) - 1; p++)
                    meta[mn++] = *p;
                size_t used = gw_json_escape_append(meta + mn, sizeof(meta) - (size_t)mn,
                                                    sctx->reasoning, rl);
                mn += (int)used;
                if (mn < (int)sizeof(meta) - 2) {
                    meta[mn++] = '"';
                }
            }
        }
        snprintf(meta + mn, sizeof(meta) - (size_t)mn, "}");
    }
    char *rid = NULL;
    airy_sys_memory_write(content, (size_t)n, meta, &rid);
    AIRY_FREE(rid);
}
