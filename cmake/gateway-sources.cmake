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
#   - 编译期特性（GATEWAY_HAS_WS/HTTP/HTTP2、AIRY_COMPLIANCE_IMPL 豁免等）
#     仍由各消费方 CMakeLists 按其目标独立设置，不属本清单职责
#
# 2026-11-25 目录原子化（0.1.21 S30-P1-4）：src/gateway 原先单层平铺 22 个
# .c，协议族与职责边界只能靠文件名前缀辨认。按「一族一目录」分层后，
# 同族实现与私头同址，跨族只经 gateway_internal.h 与公开头交互。
# 裸名 include 由 GATEWAY_COMMON_INCLUDE_DIRS 统一挂载，两侧消费方共用，
# 禁止任一消费方自行拼目录。
# ============================================================================

if(NOT DEFINED GATEWAY_SRC_BASE)
    message(FATAL_ERROR "gateway-sources.cmake: GATEWAY_SRC_BASE 必须先设置")
endif()

set(GATEWAY_COMMON_SOURCES
    # 顶层：路由入口与装配面
    ${GATEWAY_SRC_BASE}/src/gateway/gateway_api.c
    # 鉴权：入口鉴权纯策略，无 MHD 依赖可独立单测
    ${GATEWAY_SRC_BASE}/src/gateway/auth/gateway_auth.c
    # 大厅读侧：磁盘事件流扫描/游标/SSE 推送
    ${GATEWAY_SRC_BASE}/src/gateway/hall/gateway_hall_store.c
    ${GATEWAY_SRC_BASE}/src/gateway/hall/gateway_sse_hall_watch.c
    ${GATEWAY_SRC_BASE}/src/gateway/hall/gateway_sse_run_stream.c
    # HTTP/1.1 家族（含 HTTP/2 共用基类生命周期 SSoT，无条件编译）
    ${GATEWAY_SRC_BASE}/src/gateway/http1/http_gateway.c
    ${GATEWAY_SRC_BASE}/src/gateway/http1/http_gateway_base.c
    ${GATEWAY_SRC_BASE}/src/gateway/http1/http_gateway_routes.c
    ${GATEWAY_SRC_BASE}/src/gateway/http1/http_gateway_sse.c
    # 南向 A-IPC 统一客户端面（AF_UNIX 唯一合法落点，门禁 N2）
    ${GATEWAY_SRC_BASE}/src/gateway/aipc/gateway_aipc_client.c
    # http2 家族：无条件编译（缺库为空 TU，实现由 AIRY_HAS_HTTP2 守卫）
    ${GATEWAY_SRC_BASE}/src/gateway/http2/http2_gateway.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2/http2_gateway_event.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2/http2_gateway_frame.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2/http2_gateway_headers.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2/http2_gateway_response.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2/http2_gateway_route.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2/http2_gateway_session.c
    ${GATEWAY_SRC_BASE}/src/gateway/http2/http2_gateway_stream.c
    # ws 家族：生命周期/消息与请求处理/事件回调（2026-08-27 拆分）
    ${GATEWAY_SRC_BASE}/src/gateway/ws/ws_gateway.c
    ${GATEWAY_SRC_BASE}/src/gateway/ws/ws_gateway_message.c
    ${GATEWAY_SRC_BASE}/src/gateway/ws/ws_gateway_callback.c
    # stdio 家族：本地管道协议适配
    ${GATEWAY_SRC_BASE}/src/gateway/stdio/stdio_gateway.c
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
    # gateway_protocol_handler 按功能域拆分：主处理/协议转换（2026-08-27）；
    # 协议检测 0.1.19 单元六十并入 protocols 机制核 proto_detect_body。
    ${GATEWAY_SRC_BASE}/src/utils/gateway_protocol_handler.c
    ${GATEWAY_SRC_BASE}/src/utils/gateway_protocol_convert.c
)

# src/gateway 分层后的裸名 include 解析面：两侧消费方（gateway 静态库与
# gateway_d 的 gateway_lib_obj）必须挂载同一份清单，禁止各自拼目录。
set(GATEWAY_COMMON_INCLUDE_DIRS
    ${GATEWAY_SRC_BASE}/src/gateway
    ${GATEWAY_SRC_BASE}/src/gateway/auth
    ${GATEWAY_SRC_BASE}/src/gateway/aipc
    ${GATEWAY_SRC_BASE}/src/gateway/hall
    ${GATEWAY_SRC_BASE}/src/gateway/http1
    ${GATEWAY_SRC_BASE}/src/gateway/http2
    ${GATEWAY_SRC_BASE}/src/gateway/ws
    ${GATEWAY_SRC_BASE}/src/gateway/stdio
)
