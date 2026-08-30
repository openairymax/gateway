# gateway - AgentRT 网关模块
# 协议转换层：HTTP · WebSocket · Stdio
#
# Copyright (C) 2026 SPHARX. All Rights Reserved.
# SPDX-FileCopyrightText: 2026 SPHARX.
# SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(COREKERN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../atoms/corekern")
set(SYSCALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../atoms/syscall")
set(COMMONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../commons")
set(PROTOCOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../protocols")

airy_print_info("[GATEWAY] Configuring gateway module (cJSON: ${AIRY_HAS_CJSON}, microhttpd: ${AIRY_HAS_MICROHTTPD}, websockets: ${AIRY_HAS_LIBWEBSOCKETS})")

# cJSON 是网关模块的硬依赖（0.1.1 零技术债：不允许桩库）
if(NOT AIRY_HAS_CJSON)
    message(FATAL_ERROR "[GATEWAY] cJSON is a hard dependency for Airymax 0.1.1. "
                        "Please install cJSON (e.g. apt install libcjson-dev) and reconfigure. "
                        "Stub gateway library is not supported.")
endif()

# 验证关键路径
foreach(_dir ${COREKERN_DIR}/include ${SYSCALL_DIR}/include ${COMMONS_DIR} ${PROTOCOLS_DIR}/include ${PROTOCOLS_DIR}/common/include)
    if(EXISTS "${_dir}")
        airy_print_ok("[GATEWAY]   [OK] ${_dir}")
    else()
        airy_print_warn("[GATEWAY]   [MISSING] ${_dir}")
    endif()
endforeach()

if(AIRY_HAS_CJSON)
# 源码清单单一权威源（SSoT）：与 daemons/gateway_d 共用
# cmake/gateway-sources.cmake，禁止在两侧各自维护源清单。
set(GATEWAY_SRC_BASE ${CMAKE_CURRENT_SOURCE_DIR})
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/gateway-sources.cmake)
set(GATEWAY_SOURCES ${GATEWAY_COMMON_SOURCES})

# ========== 头文件 ==========
set(GATEWAY_HEADERS
    src/gateway/gateway.h
    src/gateway/gateway_hall_store.h
    src/gateway/http_gateway.h
    src/gateway/http2_gateway.h
    src/gateway/ws_gateway.h
    src/gateway/stdio_gateway.h
    src/utils/jsonrpc.h
    src/utils/syscall/syscall_router.h
    src/utils/gateway_utils.h
    src/utils/gateway_protocol_handler.h
    src/utils/gateway_rate_limiter.h
)

# ========== nghttp2 检测（HTTP/2 支持） ==========
# 头文件位于项目本地 commons/include/third_party/nghttp2/
# 运行时库可能只有 .so.14（无开发版 .so 软链接）
find_library(NGHTTP2_LIBRARY NAMES nghttp2)
if(NOT NGHTTP2_LIBRARY)
    # 回退：查找版本化运行时库文件
    find_file(NGHTTP2_LIBRARY
        NAMES libnghttp2.so.14 libnghttp2.so.19 libnghttp2.so
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib
    )
endif()
if(NGHTTP2_LIBRARY)
    set(AIRY_HAS_HTTP2 TRUE CACHE BOOL "nghttp2 available")
    airy_print_ok("[GATEWAY]   nghttp2 found: ${NGHTTP2_LIBRARY} (HTTP/2 gateway enabled)")
else()
    set(AIRY_HAS_HTTP2 FALSE CACHE BOOL "nghttp2 available")
    airy_print_warn("[GATEWAY]   nghttp2 not found, HTTP/2 gateway disabled")
endif()

# ========== 库目标 ==========
add_library(gateway STATIC ${GATEWAY_SOURCES})

# 版本 SSoT：GATEWAY_VERSION 从 agentrt 根 VERSION 文件注入（支持 0.1.5a
# 后缀），header 内 #define 仅作缺省回退（未走 CMake 的独立编译）。
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" GATEWAY_VERSION_RAW LIMIT_COUNT 1)
string(STRIP "${GATEWAY_VERSION_RAW}" GATEWAY_VERSION_RAW)
if(GATEWAY_VERSION_RAW)
    target_compile_definitions(gateway PRIVATE GATEWAY_VERSION="${GATEWAY_VERSION_RAW}")
endif()

# jsonrpc.c / syscall_router.c 等源文件使用 AIRY_HAS_CJSON 守卫
target_compile_definitions(gateway PUBLIC AIRY_HAS_CJSON=1)

if(AIRY_HAS_LIBWEBSOCKETS)
    set_source_files_properties(
        src/gateway/ws_gateway.c
        # 2026-08-27 ws_gateway 拆分：消息/回调文件经 ws_gateway_internal.h
        # 在 GATEWAY_HAS_WS 下包含 <libwebsockets.h>，同样需要豁免
        src/gateway/ws_gateway_message.c
        src/gateway/ws_gateway_callback.c
        PROPERTIES COMPILE_DEFINITIONS "AIRY_COMPLIANCE_IMPL"
    )
endif()

# nghttp2 头文件内部使用 malloc/free/memcpy/memset 等函数，
# 在 AIRY_COMPLIANCE_STRICT 模式下会被 banned_functions.h 毒化，
# 需通过 AIRY_COMPLIANCE_IMPL 豁免（与 ws_gateway.c 同策略）。
# 注意：此豁免必须无条件生效——http2_gateway*.c 始终参与编译
# （无 nghttp2 库时空 TU，实现代码由 #ifdef AIRY_HAS_HTTP2 守卫），
# 而其 #include <nghttp2/nghttp2.h> 位于守卫之前，若豁免依赖
# AIRY_HAS_HTTP2，则缺库平台会因 poisoned "malloc" 编译失败。
set_source_files_properties(
    src/gateway/http2_gateway.c
    src/gateway/http2_gateway_event.c
    src/gateway/http2_gateway_frame.c
    src/gateway/http2_gateway_headers.c
    src/gateway/http2_gateway_response.c
    src/gateway/http2_gateway_route.c
    src/gateway/http2_gateway_session.c
    src/gateway/http2_gateway_stream.c
    PROPERTIES COMPILE_DEFINITIONS "AIRY_COMPLIANCE_IMPL"
)

target_include_directories(gateway
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/utils>
        # 2026-08-27 目录原子化：syscall 路由家族子目录（裸名 include 解析）
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/utils/syscall>
        # P0.18.2: gateway_rpc_handler.h (PUBLIC 头) 引用 <cjson_helpers.h>，
        # 需向 gateway 的消费者（含测试目标）传递此 include 路径，避免找不到头文件
        $<BUILD_INTERFACE:${COMMONS_DIR}/utils/cjson/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COREKERN_DIR}/include
        ${SYSCALL_DIR}/include
        ${COMMONS_DIR}
        ${COMMONS_DIR}/utils/include
        ${COMMONS_DIR}/utils/types/include
        ${COMMONS_DIR}/utils/compat/include
        ${COMMONS_DIR}/platform/include
        ${COMMONS_DIR}/utils/error/include
        ${COMMONS_DIR}/utils/logging/include
        # S-2 收敛 (2026-08-14): AIRY_LOG_* 权威宏源
        # commons/utils/observability/include/logger.h（logging.h 统一入口已引用）
        ${COMMONS_DIR}/utils/observability/include
        ${COMMONS_DIR}/utils/memory/include
        ${COMMONS_DIR}/utils/sync/include
        ${COMMONS_DIR}/utils/compat/include
        ${COMMONS_DIR}/utils/include
        ${PROTOCOLS_DIR}/core/router/include
        ${PROTOCOLS_DIR}/core/registry/include
        ${PROTOCOLS_DIR}/core/adapter/include
        ${PROTOCOLS_DIR}/core/transformers/include
        ${PROTOCOLS_DIR}/common/include
        ${PROTOCOLS_DIR}/src
        ${PROTOCOLS_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # R-09-124: cjson (system library)
        ${CJSON_INCLUDE_DIRS}/cjson
        # nghttp2 头文件（HTTP/2 支持）— 项目本地副本
        # 头文件位于 commons/include/third_party/nghttp2/nghttp2.h
        # include 路径需指向 third_party/ 以解析 <nghttp2/nghttp2.h>
        ${COMMONS_DIR}/include/third_party
)

# P0.18.1: svc_common 提供 jsonrpc_get_error_message（jsonrpc_helpers.c），
# gateway/src/utils/jsonrpc.c:167 调用此符号，故 gateway 依赖 svc_common。
# 架构约束 2026-08-25 "必须走 syscall": gateway 的 airy_sys_* 方法面经
# airy_sys_svc_call()（SYS_SVC_CALL）派发，故 gateway 依赖 atoms/syscall
# （airy_syscall 归档；仅 svc_dispatch.o 被拉取，其余 airy_sys_* 由本库
# syscall_router_*.c 提供，避免重复符号）。
# PUBLIC 传播至测试目标（test_jsonrpc/test_syscall_router/test_rpc_handler）
target_link_libraries(gateway
    PUBLIC
        svc_common
        airy_syscall
    PRIVATE
        airy_protocols
        airy_common
        airy_memory
)

if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(gateway PUBLIC cjson)
    elseif(CJSON_FOUND)
        target_link_libraries(gateway PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()

if(AIRY_HAS_MICROHTTPD)
    # http_gateway.c 的实现包在 #ifdef GATEWAY_HAS_HTTP 内；缺此宏时
    # http_gateway_register_endpoint 等符号无定义，gateway_api.c 引用导致
    # test_gateway/gateway_d 链接失败（libgateway.a 按需拉取，需与定义同步）
    target_compile_definitions(gateway PRIVATE GATEWAY_HAS_HTTP=1)
    if(libmicrohttpd_FOUND)
        target_link_libraries(gateway PRIVATE microhttpd)
    elseif(MICROHTTPD_FOUND)
        target_link_libraries(gateway PRIVATE ${MICROHTTPD_LIBRARIES})
    endif()
endif()

if(AIRY_HAS_LIBWEBSOCKETS)
    # 优先自编译 4.3.3 的 websockets_shared imported target（libssl.so.3）
    if(TARGET websockets_shared)
        target_link_libraries(gateway PRIVATE websockets_shared)
    elseif(TARGET websockets)
        target_link_libraries(gateway PRIVATE websockets)
    elseif(LIBWEBSOCKETS_FOUND)
        target_link_libraries(gateway PRIVATE ${LIBWEBSOCKETS_LIBRARIES})
    endif()
endif()

# nghttp2 链接（HTTP/2 支持）
# 使用完整路径避免消费者链接时找不到 -lnghttp2（系统仅有 .so.14 无 .so 软链接）
if(AIRY_HAS_HTTP2 AND NGHTTP2_LIBRARY)
    target_link_libraries(gateway PRIVATE ${NGHTTP2_LIBRARY})
    target_compile_definitions(gateway PRIVATE AIRY_HAS_HTTP2=1)
endif()

# ========== 安装 ==========
install(TARGETS gateway
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES
    include/gateway.h
    DESTINATION include/agentrt/gateway
)

install(FILES
    src/gateway/gateway.h
    src/gateway/http_gateway.h
    src/gateway/http2_gateway.h
    src/gateway/ws_gateway.h
    src/gateway/stdio_gateway.h
    src/utils/jsonrpc.h
    src/utils/syscall/syscall_router.h
    DESTINATION include/agentrt/gateway
)

# ========== 测试 ==========
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARK "Build performance benchmark tool (PERF-001~003)" ON)
if(BUILD_TESTS)
    enable_testing()

    # 主网关测试 (7个用例) - 2026-08-20 重写匹配当前公共 API
    # （旧测试引用已移除的 gateway_create/gateway_create_interface 符号，
    #  原 "dual-header conflict" 实为测试过时；include/gateway.h 为公共头，
    #  src/gateway/gateway.h 为内部实现头，各自独立编译链接。）
    add_executable(test_gateway tests/test_gateway.c)
    target_compile_definitions(test_gateway PRIVATE AIRY_COMPLIANCE_IMPL)
    target_link_libraries(test_gateway gateway)
    add_test(NAME gateway_test COMMAND test_gateway)

    # syscall_router 测试 (8个用例)
    add_executable(test_syscall_router tests/test_syscall_router.c)
    target_compile_definitions(test_syscall_router PRIVATE AIRY_COMPLIANCE_IMPL)
    target_link_libraries(test_syscall_router gateway)
    add_test(NAME syscall_router_test COMMAND test_syscall_router)

    # jsonrpc 协议测试 (17个用例)
    add_executable(test_jsonrpc tests/test_jsonrpc.c)
    target_compile_definitions(test_jsonrpc PRIVATE AIRY_COMPLIANCE_IMPL)
    target_link_libraries(test_jsonrpc gateway)
    add_test(NAME jsonrpc_test COMMAND test_jsonrpc)

    # gateway_rpc_handler 统一RPC处理模块测试 (14个用例)
    add_executable(test_rpc_handler tests/test_gateway_rpc_handler.c)
    target_compile_definitions(test_rpc_handler PRIVATE AIRY_COMPLIANCE_IMPL)
    target_link_libraries(test_rpc_handler gateway)
    add_test(NAME rpc_handler_test COMMAND test_rpc_handler)

    # 性能基准测试工具 (C-W1-004 / PERF-001~003)
    if(BUILD_BENCHMARK)
        add_executable(gateway_benchmark tests/gateway_benchmark.c)
        target_compile_definitions(gateway_benchmark PRIVATE AIRY_COMPLIANCE_IMPL)
        target_include_directories(gateway_benchmark PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../commons/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../commons/utils/logging/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../commons/utils/error/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../commons/utils/memory/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../commons/utils/types/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        target_link_libraries(gateway_benchmark PRIVATE gateway Threads::Threads airy_platform_libs)

        if(AIRY_HAS_CURL)
            target_include_directories(gateway_benchmark PRIVATE ${CURL_INCLUDE_DIRS})
            target_link_libraries(gateway_benchmark PRIVATE ${CURL_LIBRARIES})
            target_compile_definitions(gateway_benchmark PRIVATE USE_CURL=1)
            airy_print_info("  gateway_benchmark: with libcurl support")
        else()
            airy_print_warn("  gateway_benchmark: simulation mode (no libcurl)")
        endif()

        airy_print_ok("  gateway_benchmark: configured (TeamC C-W1-004)")
    endif()
endif()

# ========== 格式化 ==========
find_program(CLANG_FORMAT "clang-format")
if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i -style=file ${GATEWAY_SOURCES} ${GATEWAY_HEADERS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting code with clang-format"
    )
endif()

# ========== 静态分析 ==========
find_program(CPPCHECK "cppcheck")
if(CPPCHECK)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK} --enable=all --std=c11
            --suppress=missingIncludeSystem
            --config-file=${CMAKE_CURRENT_SOURCE_DIR}/config/cppcheck.cfg
            ${GATEWAY_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running cppcheck static analysis"
    )
endif()
endif()
