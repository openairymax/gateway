# gateway — 系统协议网关层（HTTP/WS/SSE/Stdio → JSON-RPC 2.0）

> Airymax 运行时对外的统一入口：每一路入站的 HTTP、HTTP/2、WebSocket、SSE、Stdio、MCP、A2A 或 OpenAI 兼容请求，都会被翻译为统一的 JSON-RPC 2.0 调用并派发到运行时。

**语言:** [English](README.md) | 简体中文

[![Version](https://img.shields.io/badge/version-0.1.16-5a6b7e)](https://atomgit.com/openairymax/gateway)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)

- **仓库：** <https://atomgit.com/openairymax/gateway>
- **版本：** 0.1.16
- **产物：** 静态库 `gateway`

---

## 是什么

**gateway** 是 Airymax 智能体运行时的协议网关层。它位于外部客户端与运行时内核之间：接收 HTTP、HTTP/2、WebSocket、SSE、Stdio 入站流量，自适应识别线协议（JSON-RPC、MCP、A2A、OpenAI 兼容），归一为 JSON-RPC 2.0，再向内转发——要么通过 syscall 接口（`atoms/syscall`），要么通过一层轻量的业务委派层调用后端服务端点（LLM、工具执行、调度）。

它的重心是协议翻译与连接管理，而非应用逻辑：业务决策交由运行时处理。模块额外提供边界侧的横切控制——令牌桶限流、入口鉴权（默认拒绝、回环感知）、URL 净化与 CORS。

```
外部客户端 ──► gateway ──► JSON-RPC 2.0 ──► 运行时（syscall / 后端服务）
             （HTTP/HTTP2/WS/SSE/Stdio · MCP/A2A/OpenAI）
```

`gateway` 库被设计为由宿主进程（例如网关守护进程）内嵌并拉起，宿主进程持有事件循环与优雅退出生命周期。

## 能力

| 领域 | 说明 |
|------|------|
| 传输 | HTTP/1.1（libmicrohttpd）、HTTP/2（nghttp2，仅 POSIX）、WebSocket（libwebsockets）、SSE、Stdio REPL |
| 协议处理 | 自适应检测并转换 JSON-RPC 2.0、MCP、A2A、OpenAI 兼容请求体为统一的内部 JSON-RPC 2.0 形态 |
| 向内派发 | 将 JSON-RPC 方法路由至 syscall 接口，或将业务调用（`agent.run`、`tools/call`、embeddings、任务调度）转发至后端服务端点 |
| 流式 | 基于 Server-Sent Events 的聊天流、智能体运行流与 hall 事件观察 |
| 边界安全 | 入口鉴权（默认拒绝、回环感知）、令牌桶限流（按 IP / 按 API Key）、URL 净化、CORS |
| 运维 | `/health` 与 `/metrics` 端点 |

## 构成

库目标 `gateway` 由两个源码区构建：

| 区域 | 路径 | 职责 |
|------|------|------|
| 传输层 | `src/gateway/` | HTTP、HTTP/2、WebSocket、SSE、Stdio 服务器；入口鉴权；协议桥接；hall 事件读写；南向 A-IPC 客户端面 |
| 共享工具 | `src/utils/` | JSON-RPC 2.0 工具、syscall 路由、RPC 处理器、协议检测/转换、限流器 |

业务委派（统一协议入口，`agent.run`/`tools/call`/embeddings/调度转发，能力注册表，PEP 缓存）为产品件，位于 `products/gateway_biz/`，由网关守护进程宿主装配，不属于 gateway 机制核。

同一份源文件清单通过 `cmake/gateway-sources.cmake` 与网关守护进程宿主共用。

### 目录结构

```
gateway/
├── CMakeLists.txt                       # CMake 构建（静态库 `gateway`）
├── cmake/
│   └── gateway-sources.cmake            # 与守护进程宿主共用的源文件清单
├── include/
│   ├── gateway.h                        # 公共 API（生命周期 / 控制 / 查询）
├── src/
│   ├── gateway/                         # 传输层
│   │   ├── gateway_api.c                # 公共 API 实现
│   │   ├── http_gateway.[ch]            # HTTP/1.1 服务器（libmicrohttpd）
│   │   ├── http_gateway_routes.[ch]     # HTTP 静态路由表 + 入口鉴权门禁
│   │   ├── http_gateway_sse*.c          # SSE 基座 + frame/stream/run/hall/memory/tool
│   │   ├── http2_gateway*.[ch]          # HTTP/2 服务器（nghttp2，仅 POSIX）
│   │   ├── ws_gateway*.[ch]             # WebSocket 服务器（libwebsockets）
│   │   ├── stdio_gateway.[ch]           # Stdio REPL 网关
│   │   ├── gateway_auth.[ch]            # 入口鉴权
│   │   ├── gateway_hall_store.[ch]      # hall 事件记录（写入侧）
│   │   └── gateway_aipc_client.[ch]     # 南向 A-IPC 统一客户端面
│   └── utils/                           # 共享工具
│       ├── jsonrpc.[ch]                 # JSON-RPC 2.0 验证 / 响应
│       ├── syscall/                     # JSON-RPC 方法 → syscall 派发
│       ├── gateway_rpc_handler.[ch]     # 各传输共享的 RPC 处理
│       ├── gateway_protocol_*.[ch]      # 协议检测 / 转换 / 处理器
│       └── gateway_rate_limiter.[ch]    # 令牌桶限流器
├── tests/                               # 单元测试 + 基准
├── deploy/                              # Kubernetes 清单（见 deploy/README.md）
└── config/                              # 静态分析配置（见 config/README.md）
```

### HTTP 路由

静态路由表（见 `src/gateway/http_gateway_routes.c`）：

| 方法 | 路径 | 流式 | 鉴权 | 用途 |
|------|------|------|------|------|
| `POST` | `/` | 否 | 是 | JSON-RPC 2.0 入口（也承载 MCP / A2A / OpenAI 请求体） |
| `POST` | `/api/v1/agent/run/stream` | SSE | 是 | 智能体运行流 |
| `GET` | `/api/v1/hall/watch` | SSE | 否 | hall 事件观察 |
| `OPTIONS` | `*` | 否 | 否 | CORS 预检 |
| `GET` | `/health` | 否 | 否 | 存活 / 就绪 |
| `GET` | `/metrics` | 否 | 是 | 指标导出 |

## 用法

### 公共 API

公共面（`include/gateway.h`）刻意保持最小——仅生命周期。实例由
`src/gateway/*.h` 声明的传输层构造函数创建，并通过
`cmake/gateway-sources.cmake` 与守护进程宿主共享：

| 函数 | 声明位置 | 说明 |
|------|----------|------|
| `http_gateway_create(host, port)` | `src/gateway/http_gateway.h` | 创建 HTTP/1.1 网关实例 |
| `http2_gateway_create(host, port)` | `src/gateway/http2_gateway.h` | 创建 HTTP/2 网关实例（仅 POSIX） |
| `ws_gateway_create(host, port)` | `src/gateway/ws_gateway.h` | 创建 WebSocket 网关实例 |
| `stdio_gateway_create()` | `src/gateway/stdio_gateway.h` | 创建 Stdio 网关实例（start 后阻塞式 REPL） |
| `gateway_start(gw)` | `include/gateway.h` | 启动网关（HTTP/WS 非阻塞，Stdio 阻塞） |
| `gateway_destroy(gw)` | `include/gateway.h` | 销毁实例并释放资源（运行中自动停止） |

```c
#include "gateway.h"
#include "http_gateway.h"

int main(void) {
    gateway_t *gw = http_gateway_create("0.0.0.0", 8080);
    if (!gw) return 1;

    gateway_start(gw);
    /* 网关持续服务直至销毁 */

    gateway_destroy(gw);   /* 停止 + 释放 */
    return 0;
}
```

### 错误码

`GATEWAY_SUCCESS` (0)、`GATEWAY_ERROR_INVALID` (-1)、`GATEWAY_ERROR_MEMORY` (-2)、`GATEWAY_ERROR_IO` (-3)、`GATEWAY_ERROR_TIMEOUT` (-4)、`GATEWAY_ERROR_CLOSED` (-5)、`GATEWAY_ERROR_PROTOCOL` (-6)。

边界拒绝以 JSON-RPC 错误上报：鉴权失败（`-32001`）、URL 不安全（`-32002`）、超出限流（`-32004`）、解析错误（`-32700`）、方法未找到（`-32601`）、请求过大（`413`）、内部错误（`-32603`）。

## 构建

```bash
# 需要 cJSON 开发头文件（硬依赖）。
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_BENCHMARK=ON
cmake --build build --target gateway --parallel

# 测试
ctest --test-dir build --output-on-failure

# 静态分析与格式化
cmake --build build --target cppcheck
cmake --build build --target format
```

**CMake 选项：**

| 选项 | 默认值 | 作用 |
|------|--------|------|
| `BUILD_TESTS` | `ON` | 构建单元测试目标（Windows 下禁用） |
| `BUILD_BENCHMARK` | `ON` | 构建基准测试目标（仅 Linux） |

**条件编译：**

| 特性 | 宏 | 缺失时行为 |
|------|----|-----------|
| cJSON | `AIRY_HAS_CJSON` | **硬失败**——`FATAL_ERROR`（不允许桩库） |
| HTTP/1.1 | `AIRY_HAS_MICROHTTPD` → `GATEWAY_HAS_HTTP` | HTTP 网关不可用 |
| WebSocket | `AIRY_HAS_LIBWEBSOCKETS` | WebSocket 网关不可用 |
| HTTP/2 | `AIRY_HAS_HTTP2` | HTTP/2 网关不可用；**Windows 下始终关闭（仅 POSIX）** |
| libcurl | `AIRY_HAS_CURL` | 基准测试以模拟模式运行 |

**产物：** 静态库 `gateway`；公共头文件安装到 `include/agentrt/gateway`。

### 运行时配置

网关通过环境变量配置（不存在 JSON 配置文件）。

| 变量 | 用途 |
|------|------|
| `GATEWAY_API_KEY` | 入口鉴权的 API Key |
| `GATEWAY_RATE_LIMIT_ENABLED` / `_RPS` / `_RPM` | 启用限流；每秒 / 每分钟请求数 |
| `GATEWAY_CORS_MODE` / `GATEWAY_CORS_ORIGINS` | CORS 策略 |
| `GATEWAY_HTTP_CONN_LIMIT` / `GATEWAY_HTTP_TIMEOUT` / `GATEWAY_HTTP_THREADS` | HTTP/1.1 服务器调优 |
| `GATEWAY_HTTP2_MAX_STREAMS` / `GATEWAY_HTTP2_TIMEOUT` | HTTP/2 服务器调优 |
| `GATEWAY_MAX_REQUEST_SIZE` | 最大请求体大小 |
| `GATEWAY_PROTOCOL_HANDLER` | 协议处理器选择 |
| `AIRY_LLM_SOCK` / `AIRY_LLM_TCP_ADDR` / `AIRY_LLM_TCP_PORT` | LLM 后端端点 |
| `AIRY_AGENT_SOCK` / `AIRY_TOOL_SOCK` | 智能体 / 工具后端端点 |
| `AIRY_AGENT_MODEL` | 智能体运行的默认模型 |
| `AIRY_MAX_SESSIONS` / `AIRY_RATE_LIMIT_TABLE_SIZE` | 会话与限流表大小 |
| `AIRY_STDIO_BUFFER_SIZE` / `AIRY_GATEWAY_MEM_PUBLIC` | Stdio 缓冲区大小；公共内存标志 |

## 关系

**上游依赖**（本模块链接）：

| 依赖 | 提供 |
|------|------|
| [protocols](https://atomgit.com/openairymax/protocols) | 路由器 / 网关 / 注册中心接口与 A2A / MCP / OpenAI 适配器（`airy_protocols`） |
| [agentrt atoms](https://atomgit.com/openairymax/atoms) | syscall 派发目标与内存原语（`airy_memory`） |
| [agentrt commons](https://atomgit.com/openairymax/commons) | 平台、类型、错误、日志、同步基础（`airy_common`、`svc_common`、`airy_syscall`） |
| cJSON（外部） | JSON 解析——**硬依赖** |
| libmicrohttpd / libwebsockets / nghttp2 / OpenSSL | HTTP、WebSocket、HTTP/2 与 TLS 传输 |

**下游消费者：** 网关守护进程宿主封装本库并以系统服务运行；SDK 与智能体应用通过 HTTP / WebSocket / SSE / Stdio / MCP 接入。

## 许可证

Copyright (c) 2025-2026 SPHARX Ltd.

本模块为开源软件，采用双许可证，您可以选择以下任一许可证遵守：

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt))，或
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

完整许可证文本见 [LICENSE](LICENSE)，版权声明见 [NOTICE](NOTICE)。
