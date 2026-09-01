/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */

/**
 * @file test_sse_stream.c
 * @brief SSE 流式子系统单元测试（仅依赖内部头，与公共 gateway.h 独立编译，
 *        规避 include/gateway.h 与 src/gateway/gateway_internal.h 的既有
 *        枚举重复定义冲突）。
 *
 * 覆盖 0.1.8 社区缺陷：llm_d 流式失败时把整个 JSON-RPC 错误对象裸写
 * socket（无 RS 控制帧），网关旧版将其当正文 data: 帧透传，TUI
 * [Super Agent] 气泡原样显示 {"jsonrpc":...,"error":{...}}。
 * gw_sse_llm_error_envelope 须在文本转发前识别信封并提取 message，
 * 供上层转为 __airy_evt:error 事件帧。
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

static void envelope_case(const char *buf, int eof, int want, const char *want_msg,
                          const char *desc)
{
    gw_sse_ctx_t sctx;
    char msg[256];
    memset(&sctx, 0, sizeof(sctx));
    sctx.stream_buf = (char *)buf;
    sctx.stream_len = strlen(buf);
    sctx.stream_eof = eof;
    msg[0] = '\0';
    int got = gw_sse_llm_error_envelope(&sctx, msg, sizeof(msg));
    CHECK(got == want, desc);
    if (want == 1 && want_msg)
        CHECK(strcmp(msg, want_msg) == 0, "envelope message extraction");
}

int main(void)
{
    printf("[SSE Stream Tests] llm_error_envelope\n");

    /* 1. 标准错误信封 → 确认（1），message 提取 */
    envelope_case("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,"
                  "\"message\":\"No LLM provider configured\"}}",
                  0, 1, "No LLM provider configured", "full envelope detected");

    /* 2. 正常文本块 → 放行（0） */
    envelope_case("什么是最终产品", 0, 0, NULL, "plain CJK text passes");

    /* 3. 以 { 开头的普通 JSON 文本（非信封）→ 放行（0） */
    envelope_case("{\"result\":\"ok\"}", 0, 0, NULL, "non-envelope json passes");

    /* 4. 信封签名未收全（流未结束）→ 暂缓（2） */
    envelope_case("{\"json", 0, 2, NULL, "partial signature defers");

    /* 5. 签名收全但 JSON 未收全（流未结束）→ 暂缓（2） */
    envelope_case("{\"jsonrpc\":\"2.0\",\"err", 0, 2, NULL, "partial json defers");

    /* 6. 流已结束且 JSON 畸形 → 按正文处理（0），不吞文本 */
    envelope_case("{\"jsonrpc\":\"2.0\",\"err", 1, 0, NULL, "eof malformed passes as text");

    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
