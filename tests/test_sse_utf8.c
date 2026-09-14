/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */

/**
 * @file test_sse_utf8.c
 * @brief SSE 记忆子系统 UTF-8 边界回归（N-1）。
 *
 * 背景：gateway 的长期记忆注入/写回是请求体非法 UTF-8 的上游来源之一——
 * 记忆摘要或 "用户:/AgentRT:" 记录体若在多字节字符中间被字节数截断，
 * 半个汉字/emoji 会经记忆召回进入 llm_d 请求体，provider 直接回 400
 * （N-3）。本用例锁定四件事：
 *   1. gw_sse_utf8_sanitize() 把非法/截断序列归一为 U+FFFD，完整序列原样保留；
 *   2. gw_sse_utf8_safe_len() 只返回 UTF-8 安全前缀长度，且 n==avail 时不越界读；
 *   3. gw_sse_mem_format_record() 在 1600 字节上限处不切断多字节字符，产物
 *      再次 sanitize 后不变（幂等，证明无残余非法字节）；
 *   4. gw_sse_mem_build_meta() 的 reasoning 字段同样边界安全：超 600 字节时回退
 *      到字符边界、截断尾段清洗为 U+FFFD、容量不足时整条省略——产物恒为完整
 *      合法 JSON（cJSON 可解析），绝不落半截转义序列。
 *
 * 仅依赖内部头（与公共 include/gateway.h 独立编译，规避枚举重复定义冲突）。
 */

#include "gateway/http_gateway_sse_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, desc)                                                      \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("  FAIL: %s (line %d)\n", desc, __LINE__);                  \
        }                                                                      \
    } while (0)

/* ── gw_sse_utf8_sanitize ──────────────────────────────────────────── */

static void check_sanitize(const char *in, size_t len, const char *want, const char *desc)
{
    char *got = gw_sse_utf8_sanitize(in, len);
    if (!got) {
        CHECK(want == NULL, desc);
        return;
    }
    CHECK(want != NULL && strcmp(got, want) == 0, desc);
    AIRY_FREE(got);
}

static void test_sanitize(void)
{
    /* 非法入参 / 空输入 */
    check_sanitize(NULL, 0, NULL, "sanitize(NULL) -> NULL");
    check_sanitize("", 0, "", "sanitize(\"\",0) -> \"\"");

    /* 完整序列原样保留 */
    check_sanitize("hello world", 11, "hello world", "ascii preserved");
    check_sanitize("\xE4\xB8\xAD\xE6\x96\x87", 6, "中文", "complete cjk preserved");
    check_sanitize("\xF0\x9F\x98\x80", 4, "\xF0\x9F\x98\x80", "complete emoji preserved");

    /* 截断/非法序列归一为 U+FFFD（EF BF BD） */
    check_sanitize("\xE4\xB8", 2, "\xEF\xBF\xBD\xEF\xBF\xBD",
                   "truncated 3-byte cjk -> U+FFFD");
    check_sanitize("\xF0\x9F\x98", 3, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD",
                   "truncated 4-byte emoji -> U+FFFD");
    check_sanitize("\x80", 1, "\xEF\xBF\xBD", "lone continuation -> U+FFFD");
    check_sanitize("\xFF", 1, "\xEF\xBF\xBD", "invalid lead 0xFF -> U+FFFD");
    check_sanitize("\xC0", 1, "\xEF\xBF\xBD", "lead without continuation -> U+FFFD");

    /* 合法前缀 + 截断尾段：前段保留，尾段归一 */
    check_sanitize("\xE4\xB8\xAD\xE6\x96", 5, "\xE4\xB8\xAD\xEF\xBF\xBD\xEF\xBF\xBD",
                   "valid prefix + truncated tail");

    /* 性质检查：输出中除 U+FFFD 外不得出现任何 >=0x80 的非法字节 */
    {
        static const unsigned char in[] = {0xE4, 0xB8, 'A', 0x80, 0xF0, 0x9F};
        char *got = gw_sse_utf8_sanitize((const char *)in, sizeof(in));
        int ok = (got != NULL);
        for (size_t i = 0; ok && got[i]; ) {
            unsigned char c = (unsigned char)got[i];
            if (c < 0x80) {
                i++;
            } else if (c == 0xEF && (unsigned char)got[i + 1] == 0xBF &&
                       (unsigned char)got[i + 2] == 0xBD) {
                i += 3; /* U+FFFD */
            } else {
                ok = 0; /* 其它多字节内容只可能是非法残余 */
            }
        }
        CHECK(ok, "sanitize output has no raw invalid bytes");
        AIRY_FREE(got);
    }
}

/* ── gw_sse_utf8_safe_len ──────────────────────────────────────────── */

static void test_safe_len(void)
{
    /* 空入参 / 零上限 */
    CHECK(gw_sse_utf8_safe_len(NULL, 10, 10) == 0, "safe_len(NULL) == 0");
    CHECK(gw_sse_utf8_safe_len("abc", 3, 0) == 0, "safe_len(max=0) == 0");

    /* 整段在范围内：n == avail，直接返回（且不越界探测 s[avail]） */
    CHECK(gw_sse_utf8_safe_len("abc", 3, 3) == 3, "safe_len(avail==max) == avail");
    CHECK(gw_sse_utf8_safe_len("abc", 3, 100) == 3, "safe_len(max>avail) == avail");
    CHECK(gw_sse_utf8_safe_len("\xE4\xB8\xAD", 3, 100) == 3, "safe_len keeps whole cjk");

    /* "中文" = E4 B8 AD E6 96 87：上限落在序列中间时回退到字符边界 */
    CHECK(gw_sse_utf8_safe_len("\xE4\xB8\xAD\xE6\x96\x87", 6, 5) == 3,
          "safe_len backs off mid-char (max=5)");
    CHECK(gw_sse_utf8_safe_len("\xE4\xB8\xAD\xE6\x96\x87", 6, 4) == 3,
          "safe_len backs off mid-char (max=4)");
    CHECK(gw_sse_utf8_safe_len("\xE4\xB8\xAD\xE6\x96\x87", 6, 3) == 3,
          "safe_len on char boundary stays");

    /* 4 字节 emoji：上限 3 → 回退到 0；上限 4 → 保留 */
    CHECK(gw_sse_utf8_safe_len("\xF0\x9F\x98\x80", 4, 3) == 0,
          "safe_len backs off full emoji");
    CHECK(gw_sse_utf8_safe_len("\xF0\x9F\x98\x80", 4, 4) == 4,
          "safe_len keeps full emoji");

    /* ASCII + CJK："a中" = 61 E4 B8 AD，上限 3 → 回退到 ASCII 之后 */
    CHECK(gw_sse_utf8_safe_len("a\xE4\xB8\xAD", 4, 3) == 1,
          "safe_len ascii+cjk backs off to ascii");
}

/* ── gw_sse_mem_format_record ──────────────────────────────────────── */

/* 严格 UTF-8 结构校验：每个序列长度合法且续接字节合规（不校验 overlong）。 */
static int is_valid_utf8(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x80)
            need = 1;
        else if ((c & 0xE0) == 0xC0)
            need = 2;
        else if ((c & 0xF0) == 0xE0)
            need = 3;
        else if ((c & 0xF8) == 0xF0)
            need = 4;
        else
            return 0;
        if (i + need > len)
            return 0;
        for (size_t k = 1; k < need; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80)
                return 0;
        i += need;
    }
    return 1;
}

static void test_format_record(void)
{
    /* 非法入参 → NULL */
    CHECK(gw_sse_mem_format_record(NULL, "12345678") == NULL,
          "format_record(NULL prompt) == NULL");
    CHECK(gw_sse_mem_format_record("hi", NULL) == NULL, "format_record(NULL text) == NULL");
    CHECK(gw_sse_mem_format_record("", "12345678") == NULL,
          "format_record(empty prompt) == NULL");

    /* 未触上限：原样格式化 */
    {
        char *s = gw_sse_mem_format_record("hi", "12345678");
        CHECK(s && strcmp(s, "用户: hi\nAgentRT: 12345678") == 0,
              "format_record short passthrough");
        AIRY_FREE(s);
    }

    /* 上限边界：前缀 "用户: "(8) + prompt "ab"(2) + "\nAgentRT: "(10) = 20 字节；
     * (1600-20) = 1580 = 3*526 + 2，故 content[1600] 恰为第 526 个汉字的第 3
     * 字节（续接字节）——必须回退，产物不得终止于半个字符。 */
    {
        const size_t reps = 600; /* 600*3 = 1800 字节，超 1600 上限 */
        char *final_text = (char *)AIRY_MALLOC(reps * 3 + 1);
        CHECK(final_text != NULL, "alloc final_text");
        if (final_text) {
            for (size_t i = 0; i < reps; ++i) {
                final_text[i * 3 + 0] = '\xE4';
                final_text[i * 3 + 1] = '\xB8';
                final_text[i * 3 + 2] = '\xAD';
            }
            final_text[reps * 3] = '\0';

            char *s = gw_sse_mem_format_record("ab", final_text);
            CHECK(s != NULL, "format_record(long) != NULL");
            if (s) {
                size_t len = strlen(s);
                CHECK(len <= 1600, "format_record respects 1600-byte cap");
                CHECK(len >= 1592, "format_record keeps near-cap bytes");
                CHECK(is_valid_utf8(s, len), "format_record output is well-formed utf-8");
                CHECK(strstr(s, "\xEF\xBF\xBD") == NULL,
                      "format_record has no replacement char");
                CHECK(len >= 20 && ((len - 20) % 3) == 0,
                      "format_record tail aligned to char boundary");
                CHECK(len >= 23 && memcmp(s + len - 3, "\xE4\xB8\xAD", 3) == 0,
                      "format_record ends with a whole cjk char");
                /* 幂等：再 sanitize 不变 → 无残余非法字节 */
                char *again = gw_sse_utf8_sanitize(s, len);
                CHECK(again && strcmp(again, s) == 0, "format_record output is sanitized");
                AIRY_FREE(again);
                AIRY_FREE(s);
            }
            AIRY_FREE(final_text);
        }
    }

    /* 显式构造截断的尾字节（半个汉字）作为 final_text，产物须被清洗。 */
    {
        char *s = gw_sse_mem_format_record("ab", "12345678\xE4\xB8");
        CHECK(s != NULL, "format_record(truncated tail) != NULL");
        if (s) {
            char *again = gw_sse_utf8_sanitize(s, strlen(s));
            CHECK(again && strcmp(again, s) == 0, "truncated tail is sanitized");
            AIRY_FREE(again);
            AIRY_FREE(s);
        }
    }
}

/* ── gw_sse_mem_build_meta ─────────────────────────────────────────── */

/* 用 n 个 "中"(E4 B8 AD) 填充 buf[off..]，返回写入字节数（不含 NUL 位置）。 */
static size_t fill_cjk(char *buf, size_t off, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        buf[off + i * 3 + 0] = '\xE4';
        buf[off + i * 3 + 1] = '\xB8';
        buf[off + i * 3 + 2] = '\xAD';
    }
    return off + n * 3;
}

/* 解析 metadata 并返回 reasoning 字符串（无键返回 NULL）；调用方负责 cJSON_Delete。 */
static const char *meta_reasoning(cJSON *o, size_t *out_len)
{
    if (!o)
        return NULL;
    cJSON *r = cJSON_GetObjectItem(o, "reasoning");
    if (!r || !cJSON_IsString(r) || !r->valuestring)
        return NULL;
    if (out_len)
        *out_len = strlen(r->valuestring);
    return r->valuestring;
}

static void test_build_meta(void)
{
    char meta[1024];
    gw_sse_ctx_t sctx;

    /* 非法入参 */
    memset(&sctx, 0, sizeof(sctx));
    CHECK(gw_sse_mem_build_meta(NULL, sizeof(meta), &sctx) == -1,
          "build_meta(NULL meta) == -1");
    CHECK(gw_sse_mem_build_meta(meta, sizeof(meta), NULL) == -1, "build_meta(NULL ctx) == -1");
    CHECK(gw_sse_mem_build_meta(meta, 1, &sctx) == -1, "build_meta(cap<2) == -1");

    /* 无 reasoning：仍是完整合法 JSON，不含 reasoning 键，其余字段在位 */
    {
        int mn = gw_sse_mem_build_meta(meta, sizeof(meta), &sctx);
        CHECK(mn > 0 && mn == (int)strlen(meta), "build_meta(no reasoning) returns exact length");
        CHECK(mn > 0 && meta[mn - 1] == '}', "build_meta(no reasoning) closes brace");
        cJSON *o = cJSON_Parse(meta);
        CHECK(o != NULL, "build_meta(no reasoning) is valid json");
        CHECK(o && cJSON_GetObjectItem(o, "reasoning") == NULL,
              "build_meta(no reasoning) omits key");
        CHECK(o && cJSON_GetObjectItem(o, "source") != NULL,
              "build_meta(no reasoning) keeps source");
        cJSON_Delete(o);
    }

    /* 超上限（300 汉字 = 900 字节）：回退到 600 字节整字符边界，无 U+FFFD */
    {
        const size_t reps = 300;
        char *reason = (char *)AIRY_MALLOC(reps * 3 + 1);
        CHECK(reason != NULL, "alloc long reasoning");
        if (reason) {
            fill_cjk(reason, 0, reps);
            reason[reps * 3] = '\0';
            memset(&sctx, 0, sizeof(sctx));
            sctx.reasoning = reason;

            int mn = gw_sse_mem_build_meta(meta, sizeof(meta), &sctx);
            CHECK(mn > 0 && mn == (int)strlen(meta), "build_meta(long) returns exact length");
            cJSON *o = cJSON_Parse(meta);
            CHECK(o != NULL, "build_meta(long) is valid json");
            size_t rlen = 0;
            const char *r = meta_reasoning(o, &rlen);
            CHECK(r != NULL, "build_meta(long) keeps reasoning");
            if (r) {
                CHECK(rlen == 600, "build_meta clamps reasoning to 600 bytes");
                CHECK(is_valid_utf8(r, rlen), "reasoning is well-formed utf-8");
                CHECK(strstr(r, "\xEF\xBF\xBD") == NULL, "reasoning has no replacement char");
                CHECK(rlen >= 3 && memcmp(r + rlen - 3, "\xE4\xB8\xAD", 3) == 0,
                      "reasoning ends on char boundary");
            }
            cJSON_Delete(o);
            AIRY_FREE(reason);
        }
    }

    /* 上限落在字符中间（"ab" + 300 汉字）：回退到 599 字节字符边界 */
    {
        const size_t reps = 300;
        char *reason = (char *)AIRY_MALLOC(2 + reps * 3 + 1);
        CHECK(reason != NULL, "alloc misaligned reasoning");
        if (reason) {
            reason[0] = 'a';
            reason[1] = 'b';
            size_t end = fill_cjk(reason, 2, reps);
            reason[end] = '\0';
            memset(&sctx, 0, sizeof(sctx));
            sctx.reasoning = reason;

            int mn = gw_sse_mem_build_meta(meta, sizeof(meta), &sctx);
            CHECK(mn > 0, "build_meta(misaligned) > 0");
            cJSON *o = cJSON_Parse(meta);
            CHECK(o != NULL, "build_meta(misaligned) is valid json");
            size_t rlen = 0;
            const char *r = meta_reasoning(o, &rlen);
            CHECK(r != NULL, "build_meta(misaligned) keeps reasoning");
            if (r) {
                /* 2 + 199*3 = 599：600 落在第 200 个汉字内部，须回退 */
                CHECK(rlen == 599, "build_meta backs off mid-char (599 bytes)");
                CHECK(is_valid_utf8(r, rlen), "misaligned reasoning is well-formed utf-8");
                CHECK(strstr(r, "\xEF\xBF\xBD") == NULL,
                      "misaligned reasoning has no replacement char");
                CHECK(rlen >= 3 && memcmp(r + rlen - 3, "\xE4\xB8\xAD", 3) == 0,
                      "misaligned reasoning ends on char boundary");
            }
            cJSON_Delete(o);
            AIRY_FREE(reason);
        }
    }

    /* 尾部半截汉字：清洗为 U+FFFD（而非裸非法字节），JSON 仍合法且幂等 */
    {
        const size_t whole = 199; /* 199*3 = 597 字节完整汉字 */
        char *reason = (char *)AIRY_MALLOC(whole * 3 + 2 + 1);
        CHECK(reason != NULL, "alloc truncated-tail reasoning");
        if (reason) {
            size_t end = fill_cjk(reason, 0, whole);
            reason[end + 0] = '\xE4'; /* 半个汉字（缺续接字节） */
            reason[end + 1] = '\xB8';
            reason[end + 2] = '\0';
            memset(&sctx, 0, sizeof(sctx));
            sctx.reasoning = reason;

            int mn = gw_sse_mem_build_meta(meta, sizeof(meta), &sctx);
            CHECK(mn > 0 && mn == (int)strlen(meta), "build_meta(truncated tail) exact length");
            cJSON *o = cJSON_Parse(meta);
            CHECK(o != NULL, "build_meta(truncated tail) is valid json");
            size_t rlen = 0;
            const char *r = meta_reasoning(o, &rlen);
            CHECK(r != NULL, "build_meta(truncated tail) keeps reasoning");
            if (r) {
                CHECK(is_valid_utf8(r, rlen), "truncated tail sanitized to well-formed utf-8");
                CHECK(strstr(r, "\xEF\xBF\xBD") != NULL,
                      "truncated tail becomes U+FFFD");
                char *again = gw_sse_utf8_sanitize(r, rlen);
                CHECK(again && strcmp(again, r) == 0, "reasoning sanitize is idempotent");
                AIRY_FREE(again);
            }
            cJSON_Delete(o);
            AIRY_FREE(reason);
        }
    }

    /* 容量不足：整条省略 reasoning，metadata 仍是完整合法 JSON */
    {
        const size_t reps = 300;
        char *reason = (char *)AIRY_MALLOC(reps * 3 + 1);
        CHECK(reason != NULL, "alloc reasoning for small cap");
        if (reason) {
            fill_cjk(reason, 0, reps);
            reason[reps * 3] = '\0';
            memset(&sctx, 0, sizeof(sctx));
            sctx.reasoning = reason;

            char small[220];
            int smn = gw_sse_mem_build_meta(small, sizeof(small), &sctx);
            CHECK(smn > 0 && smn == (int)strlen(small),
                  "build_meta(small cap) returns exact length");
            CHECK(smn > 0 && small[smn - 1] == '}', "build_meta(small cap) closes brace");
            cJSON *o = cJSON_Parse(small);
            CHECK(o != NULL, "build_meta(small cap) is valid json");
            CHECK(o && cJSON_GetObjectItem(o, "reasoning") == NULL,
                  "build_meta(small cap) omits reasoning entirely");
            cJSON_Delete(o);
            AIRY_FREE(reason);
        }
    }
}

int main(void)
{
    printf("[SSE UTF-8 Boundary Tests]\n");
    test_sanitize();
    test_safe_len();
    test_format_record();
    test_build_meta();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
