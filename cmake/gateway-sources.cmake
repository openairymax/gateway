# ============================================================================
# gateway-sources.cmake - gateway 模块源码清单单一权威源（SSoT）
#
# 背景（2026-08-27 修复）：gateway 模块（gateway/CMakeLists.txt 的
# GATEWAY_SOURCES）与 gateway_d（daemons/gateway_d/CMakeLists.txt 的
# GATEWAY_LIB_SOURCES）各维护一份几乎相同的源清单。双份清单必然漂移：
# 拆分新增源文件只改一侧即触发链接失败（create_error_result 未定义）。
# 本文件将清单收敛为单一权威源，两侧 include() 后取 GATEWAY_COMMON_SOURCES。
#
# 使用约定：
#   - include 前必须先设置 GATEWAY_SRC_BASE 为 gateway 模块根目录
#   - 新增/删除 gateway 模块源文件只允许改本文件，禁止在两侧各自维护
#
# 说明：
#   - http2_gateway*.c 无条件参与编译：无 nghttp2 库时为空 TU
#     （实现体由 #ifdef AIRY_HAS_HTTP2 守卫），两侧统一走此清单
#   - gateway_protocol_bridge.c 属 gateway 模块，两侧统一包含
#   - 编译期特性（GATEWAY_HAS_WS/HTTP/HTTP2、AIRY_COMPLIANCE_IMPL 豁免等）
#     仍由各消费方 CMakeLists 按其目标独立设置，不属本清单职责
# ============================================================================

if(NOT DEFINED GATEWAY_SRC_BASE)
    message(FATAL_ERROR "gateway-sources.cmake: GATEWAY_SRC_BASE 必须先设置")
endif()

set(GATEWAY_COMMON_SOURCES
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_api.c
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_hall_store.c
    ${GATEWAY_SRC_BASE}/src/gateway/http_gateway.c
    ${GATEWAY_SRC_BASE}/src/gateway/http_gateway_routes.c
    ${GATEWAY_SRC_BASE}/src/gateway/http_gateway_sse.c
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_sse_frame.c
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_sse_stream.c
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_sse_tool.c
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_sse_memory.c
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_sse_hall_watch.c
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_sse_run_stream.c
    # http2 家族：无条件编译（缺库为空 TU，实现由 AIRY_HAS_HTTP2 守卫）
    ${GATEWAY_SRC_BASE}/src/gateway/http2_gateway.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2_gateway_event.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2_gateway_frame.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2_gateway_headers.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2_gateway_response.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2_gateway_route.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2_gateway_session.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2_gateway_stream.c
    # ws_gateway 按功能域拆分：生命周期/消息与请求处理/事件回调（2026-08-27）
    ${GATEWAY_SRC_BASE}/src/gateway/ws_gateway.c
    ${GATEWAY_SRC_BASE}/src/gateway/ws_gateway_message.c
    ${GATEWAY_SRC_BASE}/src/gateway/ws_gateway_callback.c
    ${GATEWAY_SRC_BASE}/src/gateway/stdio_gateway.c
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_protocol_bridge.c
    ${GATEWAY_SRC_BASE}/src/utils/jsonrpc.c
    # 2026-08-27 目录原子化：syscall 路由家族独立子目录
    ${GATEWAY_SRC_BASE}/src/utils/syscall/syscall_router.c
    ${GATEWAY_SRC_BASE}/src/utils/syscall/syscall_router_agent.c
    ${GATEWAY_SRC_BASE}/src/utils/syscall/syscall_router_memory.c
    ${GATEWAY_SRC_BASE}/src/utils/syscall/syscall_router_runtime.c
    ${GATEWAY_SRC_BASE}/src/utils/syscall/syscall_router_session.c
    ${GATEWAY_SRC_BASE}/src/utils/syscall/syscall_router_task.c
    ${GATEWAY_SRC_BASE}/src/utils/syscall/syscall_router_telemetry.c
    ${GATEWAY_SRC_BASE}/src/utils/gateway_rpc_handler.c
    ${GATEWAY_SRC_BASE}/src/utils/gateway_rate_limiter.c
    # gateway_protocol_handler 按功能域拆分：主处理/协议转换/协议检测（2026-08-27）
    ${GATEWAY_SRC_BASE}/src/utils/gateway_protocol_handler.c
    ${GATEWAY_SRC_BASE}/src/utils/gateway_protocol_convert.c
    ${GATEWAY_SRC_BASE}/src/utils/gateway_protocol_detect.c
)

# ============================================================================
# GATEWAY_BIZ_SOURCES - gateway 业务处理器源码清单（P9 归一，2026-09-02）
#
# 背景：biz 处理器源码原散布在 daemons/gateway_d/src/（gateway_biz_*.c 9 文件），
# 与 gateway/src/gateway/ 的 SSE 翻译分属两个源码域。M1-1d P9 归一将 biz
# 物理并入 gateway 库目录（src/biz/），本清单为唯一权威源。
#
# 消费方：仅 daemons/gateway_d 的 airy_gateway_service 目标（依赖 daemon 公共层：
# svc_common/daemon_security 等，不属于纯 gateway 库）。新增/删除 biz 源文件
# 只允许改本清单，禁止在 gateway_d/CMakeLists.txt 各维护一份。
# ============================================================================
set(GATEWAY_BIZ_SOURCES
    ${GATEWAY_SRC_BASE}/src/biz/gateway_biz_agent.c
    ${GATEWAY_SRC_BASE}/src/biz/gateway_biz_backend.c
    ${GATEWAY_SRC_BASE}/src/biz/gateway_biz_forward.c
    ${GATEWAY_SRC_BASE}/src/biz/gateway_biz_hall.c
    ${GATEWAY_SRC_BASE}/src/biz/gateway_biz_svcdispatch.c
    ${GATEWAY_SRC_BASE}/src/biz/gateway_biz_tools.c
    ${GATEWAY_SRC_BASE}/src/biz/gateway_business_handler.c
    ${GATEWAY_SRC_BASE}/src/biz/gateway_cap_registry.c
    ${GATEWAY_SRC_BASE}/src/biz/gateway_svc_adapter.c
    # M2-S5 (0.1.9 §3.2 PEP)：裁定缓存（epoch 失效键），gateway 热路径
    ${GATEWAY_SRC_BASE}/src/biz/gateway_pep_cache.c
)
