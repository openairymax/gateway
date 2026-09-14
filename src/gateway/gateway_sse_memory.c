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

#include "airy_string.h"

/* ── UTF-8 sanitiser ───────────────────────────────────────────────── */

/* 统一基础库 string_utf8_sanitize() 是全仓唯一的 UTF-8 清洗权威实现
 * （RFC 3629 全量判定：截断尾序列、overlong、代理区、>U+10FFFF 一律替换
 * U+FFFD）。本函数仅保留 gateway 内既有的「malloc 返回串」签名形状，
 * 判定逻辑全部委托权威实现，禁止在此复制第二套状态机。 */
char *gw_sse_utf8_sanitize(const char *s, size_t len)
{
    if (!s)
        return NULL;
    char *out = (char *)AIRY_MALLOC(len * 3 + 1);
    if (!out)
        return NULL;
    size_t written = string_utf8_sanitize(s, len, out, len * 3 + 1);
    out[written] = '\0';
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

/* 返回不超过 avail/max_bytes 的最长 UTF-8 安全前缀长度（不切断多字节
 * 序列）。rec 由 mem_d 返回，是 (data,len) 长度前缀缓冲、不保证 NUL
 * 结尾，故同时受 avail 约束。与 CLI 侧 cli_utf8_safe_len() 同口径：
 * 注入记忆摘要先做边界回退，再经 gw_sse_utf8_sanitize() 清洗——避免
 * 把汉字/emoji 切成半个字符（既不产生非法字节，也不产生 U+FFFD 噪声）。 */
size_t gw_sse_utf8_safe_len(const char *s, size_t avail, size_t max_bytes)
{
    if (!s)
        return 0;
    size_t n = avail < max_bytes ? avail : max_bytes;
    /* n == avail：整段都在范围内，末尾没有"下一个字节"可探测，直接返回，
     * 避免读 s[avail] 越界（s 为 (data,len) 长度前缀缓冲，不保证 NUL 结尾）。 */
    if (n >= avail)
        return n;
    /* n < avail：s[n] 若为续接字节（10xxxxxx），说明 n 落在某个多字节序列
     * 中间，回退到该序列首字节处——产出的前缀不切断汉字/emoji。 */
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

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
            size_t n = gw_sse_utf8_safe_len(rec, dlen, 200);
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

/* 记录体字节上限 / 组装缓冲（缓冲需容纳 snprintf 全文再按上限裁剪）。 */
#define GW_SSE_MEM_RECORD_MAX 1600
#define GW_SSE_MEM_RECORD_BUF 1800
/* metadata.reasoning 字节上限（仅存档展示，不参与检索，故独立收紧）。 */
#define GW_SSE_REASONING_MAX 600

/* 组装写回 mem_d 的记录体 "用户: …\nAgentRT: …"：
 *   1) 上限 GW_SSE_MEM_RECORD_MAX 字节，超长按 UTF-8 边界回退，不切断
 *      汉字/emoji（与注入侧 gw_sse_utf8_safe_len 同口径）；
 *   2) 跨进程数据来源不可信，再做一次 gw_sse_utf8_sanitize() 清洗残余非法
 *      序列——非法 UTF-8 落盘后被召回注入请求体，会让 provider 直接回 400
 *      （N-3），故此处是上游防线。
 * 返回堆分配字符串（调用方 AIRY_FREE），参数非法/分配失败返回 NULL。 */
char *gw_sse_mem_format_record(const char *user_prompt, const char *final_text)
{
    if (!user_prompt || !user_prompt[0] || !final_text || !final_text[0])
        return NULL;

    char content[GW_SSE_MEM_RECORD_BUF];
    int n = snprintf(content, sizeof(content), "用户: %s\nAgentRT: %s", user_prompt, final_text);
    if (n <= 0)
        return NULL;

    /* snprintf 返回值是"本应写入"的长度，可能大于缓冲区——实际可读字节数
     * 受 sizeof(content)-1 约束，须取二者较小值作为 avail。 */
    size_t avail = ((size_t)n < sizeof(content)) ? (size_t)n : (sizeof(content) - 1);
    size_t safe = gw_sse_utf8_safe_len(content, avail, (size_t)GW_SSE_MEM_RECORD_MAX);
    return gw_sse_utf8_sanitize(content, safe);
}

/* JSON 转义后长度（与 gw_json_escape_append 同规则；仅用于容量预判）。 */
static size_t gw_json_escape_len(const char *src, size_t len)
{
    size_t n = 0;
    if (!src)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t')
            n += 2;
        else if (c < 0x20)
            n += 6; /* \u00xx */
        else
            n += 1;
    }
    return n;
}

/* 组装写回 mem_d 的 metadata JSON。写入总长（不含 NUL）成功返回，
 * cap 不足/参数非法返回 -1。
 *
 * reasoning 为上游流式增量拼装结果，跨进程/跨版本数据不可信；
 * gw_json_escape_append() 对 >=0x20 的字节原样拷贝、不做 UTF-8 校验，
 * 若直接按字节截断到 GW_SSE_REASONING_MAX，半个汉字/emoji 会进入
 * metadata JSON，落盘后被召回注入请求体即触发 provider 400（与写回侧
 * gw_sse_mem_format_record 同类）。故先做边界回退到不超过上限的最长
 * 合法前缀，再清洗残余非法序列——与注入侧/写回侧同口径，零新增 API。
 *
 * 容量不足时整条省略 reasoning（reasoning 仅存档展示、不参与检索），
 * 保证 metadata 永远是完整合法 JSON，绝不落半截转义序列。
 *
 * 非 static：与 gw_sse_mem_format_record 同为回归用例的测试缝（仅声明于
 * 子系统私有头，不对外暴露）。 */
int gw_sse_mem_build_meta(char *meta, size_t cap, const gw_sse_ctx_t *sctx)
{
    if (!meta || !sctx || cap < 2)
        return -1;
    int mn = snprintf(meta, cap,
                      "{\"source\":\"gateway\",\"kind\":\"chat\","
                      "\"prompt_tokens\":%llu,\"completion_tokens\":%llu,"
                      "\"total_tokens\":%llu,\"cost_usd\":%.6f",
                      sctx->prompt_tokens, sctx->completion_tokens, sctx->total_tokens,
                      sctx->cost_usd);
    /* snprintf 返回"本应写入"长度，可能大于 cap；须为收尾 '}' 与 NUL 预留 2 字节。 */
    if (mn < 0 || (size_t)mn + 2 > cap)
        return -1;

    if (sctx->reasoning && sctx->reasoning[0]) {
        size_t rl = strlen(sctx->reasoning);
        size_t safe = gw_sse_utf8_safe_len(sctx->reasoning, rl, GW_SSE_REASONING_MAX);
        char *rclean = gw_sse_utf8_sanitize(sctx->reasoning, safe);
        if (rclean) {
            static const char RK[] = "reasoning\":\"";
            const size_t rklen = sizeof(RK) - 1;
            size_t clen = strlen(rclean);
            size_t elen = gw_json_escape_len(rclean, clen);
            /* 预留键名 + 关闭引号 + 结尾 '}'；另留 6 字节余量，使
             * gw_json_escape_append 的 o+6<cap 收敛条件不会在"刚好装下"
             * 时提前截断。装不下则整条省略，不落半截 JSON。 */
            if ((size_t)mn + 2 + rklen + elen + 2 + 6 <= cap) {
                meta[mn++] = ',';
                meta[mn++] = '"';
                for (const char *p = RK; *p; p++)
                    meta[mn++] = *p;
                size_t used = gw_json_escape_append(meta + mn, cap - (size_t)mn - 2, rclean,
                                                    clen);
                mn += (int)used;
                meta[mn++] = '"';
            }
            AIRY_FREE(rclean);
        }
    }
    meta[mn++] = '}';
    meta[mn] = '\0';
    return mn;
}

void gw_sse_mem_record(gw_sse_ctx_t *sctx)
{
    if (!sctx || sctx->mem_recorded || !sctx->user_prompt || !sctx->user_prompt[0])
        return;
    sctx->mem_recorded = 1;
    if (!sctx->final_text || strlen(sctx->final_text) < 8)
        return;

    char *clean = gw_sse_mem_format_record(sctx->user_prompt, sctx->final_text);
    if (!clean)
        return;

    char meta[1024];
    if (gw_sse_mem_build_meta(meta, sizeof(meta), sctx) < 0) {
        AIRY_FREE(clean);
        return;
    }
    char *rid = NULL;
    airy_sys_memory_write(clean, strlen(clean), meta, &rid);
    AIRY_FREE(clean);
    AIRY_FREE(rid);
}
