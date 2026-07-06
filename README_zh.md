**语言:** [English](README.md) | 简体中文

# Airymax Gateway — HTTP/gRPC 网关守护进程

`agentrt/gateway/`

**版本：** 0.1.1
**许可证：** AGPL-3.0-or-later OR Apache-2.0（双许可证）
**分支：** `feature/official-hubs-01`

---

## 1. 模块定位

Gateway 是 Airymax 智能体运行时的**协议网关层**。它是连接外部世界与 Airymax
内核的桥梁：接收外部 HTTP、WebSocket、Stdio 请求，统一翻译为内部
JSON-RPC 2.0 调用，再通过 `agentrt/atoms/syscall` 派发至内核服务。

Gateway 严格遵循 **K-1 内核极简** 原则——**只做协议翻译，零业务逻辑**。
所有业务决策都通过 syscall 接口委托给内核。

```
外部客户端 → gateway_d → gateway → atoms/syscall → 后端服务
              (守护进程)  (库)      (内核)
```

设计目标：

- **协议转换** —— 将 HTTP / WebSocket / Stdio 请求转换为内部 JSON-RPC 2.0 调用。
- **多协议支持** —— 自适应检测并转换 JSON-RPC、MCP、A2A、OpenAI API 协议。
- **连接管理** —— 管理客户端长连接、WebSocket 会话和并发请求。
- **安全防护** —— 基于令牌桶的速率限制、请求鉴权、CORS 策略。
- **高并发** —— 基于 libmicrohttpd 和 libwebsockets 的事件驱动架构。
- **可扩展** —— 支持动态端点注册与自定义请求处理器注入。

---

## 2. 目录结构

```
gateway/
├── CMakeLists.txt                       # CMake 构建配置
├── README.md                            # 英文版
├── README_zh.md                         # 本文件（中文）
├── LICENSE                              # 双许可证文本（AGPL-3.0 + Apache-2.0）
├── NOTICE                               # 版权声明
├── include/                             # 公共头文件
│   ├── gateway.h                        # 网关统一公共接口（生命周期/控制/查询）
│   └── gateway_protocol_bridge.h        # 网关与协议系统的桥接层接口
├── src/
│   ├── gateway/                         # 核心网关实现
│   │   ├── gateway.h                    # 内部网关头文件（ops 表、结构体定义）
│   │   ├── gateway_internal.h           # 内部类型与函数声明
│   │   ├── gateway_api.c                # 公共 API（create/start/stop/destroy）
│   │   ├── http_gateway.c/.h            # HTTP 网关（基于 libmicrohttpd）
│   │   ├── http_gateway_routes.c/.h     # HTTP 静态路由表
│   │   ├── ws_gateway.c/.h              # WebSocket 网关（基于 libwebsockets）
│   │   ├── stdio_gateway.c/.h           # Stdio 网关（REPL 交互模式）
│   │   └── gateway_protocol_bridge.c    # 协议桥接层实现
│   └── utils/                           # 工具模块
│       ├── jsonrpc.c/.h                 # JSON-RPC 2.0 工具（验证/响应/批量）
│       ├── syscall_router.c/.h          # JSON-RPC 方法 → syscall 派发
│       ├── gateway_rpc_handler.c/.h     # HTTP/WS/Stdio 共享的 RPC 处理逻辑
│       ├── gateway_protocol_handler.c/.h  # MCP/A2A/OpenAI 自适应检测与转换
│       ├── gateway_rate_limiter.c/.h    # 基于令牌桶的速率限制
│       ├── gateway_utils.h              # 通用工具宏与内联函数
│       ├── gateway_compat.h             # 兼容性定义
│       └── mcp_server.c/.h              # MCP 协议服务端（工具/资源/提示词）
├── tests/                               # 测试与基准
│   ├── test_gateway.c                   # 网关主测试（7 个用例）
│   ├── test_jsonrpc.c                   # JSON-RPC 协议测试（17 个用例）
│   ├── test_syscall_router.c            # 系统调用路由测试（8 个用例）
│   ├── test_gateway_rpc_handler.c       # RPC 处理模块测试（14 个用例）
│   └── gateway_benchmark.c              # 性能基准测试
├── docker/                              # Docker 部署（Dockerfile、compose、监控）
├── deploy/                              # K8s 部署（namespace/configmap/deployment/service）
└── config/                              # 静态分析配置（cppcheck.cfg）
```

### 核心组件

| 组件 | 文件 | 职责 |
|------|------|------|
| **公共接口** | `include/gateway.h` | 统一生命周期 API：`create / start / stop / destroy` |
| **HTTP 网关** | `http_gateway.c` | 基于 libmicrohttpd 的 HTTP 服务器，支持动态端点注册 |
| **WebSocket 网关** | `ws_gateway.c` | 基于 libwebsockets 的双向 RPC 通信 |
| **Stdio 网关** | `stdio_gateway.c` | 标准输入输出的 REPL 交互模式，阻塞式单线程 |
| **协议桥接** | `gateway_protocol_bridge.c` | Gateway ↔ Protocols 模块桥接，支持协议自动检测 |

### 工具模块

| 组件 | 文件 | 职责 |
|------|------|------|
| **JSON-RPC 2.0** | `jsonrpc.c` | 请求验证、响应生成、批量请求、通知支持 |
| **系统调用路由** | `syscall_router.c` | JSON-RPC 方法名到 syscall 函数的路由分发 |
| **RPC 处理器** | `gateway_rpc_handler.c` | 三种网关共享的统一 RPC 处理逻辑 |
| **多协议处理器** | `gateway_protocol_handler.c` | MCP / A2A / OpenAI 协议自适应检测与转换 |
| **速率限制器** | `gateway_rate_limiter.c` | 基于令牌桶算法，支持按 IP/API Key 限流 |
| **MCP 服务器** | `mcp_server.c` | MCP 协议服务端，支持工具/资源/提示词注册 |

### 架构

```
外部客户端
  │
  ├─ HTTP REST ───→ http_gateway ──→ JSON-RPC 2.0 ──→ syscall_router ──→ 后端服务
  ├─ WebSocket ──→ ws_gateway ────→ JSON-RPC 2.0 ──→ syscall_router ──→ 后端服务
  ├─ Stdio ──────→ stdio_gateway ─→ JSON-RPC 2.0 ──→ syscall_router ──→ 后端服务
  └─ MCP ────────→ mcp_server ────→ JSON-RPC 2.0 ──→ syscall_router ──→ 后端服务
                         │
              gateway_protocol_handler
              (协议检测/转换/统一处理)
                         │
              gateway_rpc_handler
              (统一 RPC 处理逻辑)
                         │
                  IPC Service Bus
```

---

## 3. 上游 / 下游依赖关系

### 上游（Gateway 依赖）

| 依赖 | 来源 | 用途 |
|------|------|------|
| **protocols** | `agentrt/protocols/` | 协议路由器/网关/注册中心接口；A2A / MCP / OpenAI 适配器——链接为 `agentrt_protocols` |
| **atoms** | `agentrt/atoms/` | CoreKern 类型与 IPC 原语；`atoms/syscall` 是 `syscall_router` 的派发目标；`atoms/memory` 链接为 `agentrt_memory` |
| **commons** | `agentrt/commons/` | 平台抽象、类型、错误框架、日志、同步、内存宏——链接为 `agentrt_common` |
| **cupolas** | `agentrt/cupolas/` | 逻辑上游：网关在协议边界调用 Cupolas 进行请求鉴权和输入净化 |
| cJSON | 外部 | JSON 解析——Airymax 0.1.1 起**硬依赖**（不允许桩库） |
| libmicrohttpd | 外部 | HTTP 服务器（≥ 0.9.70） |
| libwebsockets | 外部 | WebSocket 支持（≥ 4.3.0） |
| OpenSSL | 外部 | TLS/SSL（≥ 1.1.1） |
| libcurl | 外部 | 基准测试的 HTTP 客户端（可选；缺失时基准回退到模拟模式） |

### 下游（消费 Gateway）

| 消费者 | 用途 |
|--------|------|
| **gateway_d** | 网关守护进程（`agentrt/daemons/gateway_d/`）封装本库并以系统服务形式暴露 |
| SDK / 外部客户端 | SDK 内置网关客户端库；外部客户端通过 HTTP / WebSocket / Stdio / MCP 接入 |
| Agent 应用 | Agent 应用通过网关的 JSON-RPC 2.0 表面调用运行时 |

---

## 4. 公共 API

### 生命周期

| 函数 | 说明 |
|------|------|
| `gateway_http_create(host, port)` | 创建 HTTP 网关实例 |
| `gateway_ws_create(host, port)` | 创建 WebSocket 网关实例 |
| `gateway_stdio_create()` | 创建 Stdio 网关实例 |
| `gateway_destroy(gw)` | 销毁网关实例并释放资源 |
| `gateway_start(gw)` | 启动网关（HTTP/WS 非阻塞，Stdio 阻塞） |
| `gateway_stop(gw)` | 优雅停止网关 |

### 控制与查询

| 函数 | 说明 |
|------|------|
| `gateway_set_handler(gw, handler, user_data)` | 设置自定义请求处理回调 |
| `gateway_register_endpoint(gw, method, path, handler, user_data)` | 注册动态 HTTP 端点 |
| `gateway_get_type(gw)` | 获取网关类型枚举 |
| `gateway_is_running(gw)` | 检查网关是否运行中 |
| `gateway_get_stats(gw, out_json)` | 获取 JSON 格式统计信息 |
| `gateway_get_name(gw)` | 获取网关名称 |

### 错误码

`GATEWAY_SUCCESS` (0)、`GATEWAY_ERROR_INVALID` (-1)、`GATEWAY_ERROR_MEMORY` (-2)、
`GATEWAY_ERROR_IO` (-3)、`GATEWAY_ERROR_TIMEOUT` (-4)、`GATEWAY_ERROR_CLOSED` (-5)、
`GATEWAY_ERROR_PROTOCOL` (-6)。

### 使用示例

```c
#include "gateway.h"

int main(void) {
    gateway_t *gw = gateway_http_create("0.0.0.0", 8080);
    if (!gw) return 1;

    gateway_start(gw);

    char *stats = NULL;
    gateway_get_stats(gw, &stats);
    printf("Stats: %s\n", stats);
    free(stats);

    gateway_stop(gw);
    gateway_destroy(gw);
    return 0;
}
```

---

## 5. 构建说明

```bash
# 构建 Gateway 模块（需要 cJSON 开发库）
cmake -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARK=ON
cmake --build build --target gateway

# 运行测试
ctest --test-dir build -R "gateway|jsonrpc|syscall|rpc_handler"

# 运行性能基准
./build/gateway_benchmark

# 静态分析与格式化（如目标存在）
cmake --build build --target cppcheck
cmake --build build --target format
```

### CMake 选项与条件编译

| 依赖 | 条件宏 | 缺失时行为 |
|------|--------|-----------|
| cJSON | `AGENTRT_HAS_CJSON` | **硬失败**——网关模块以 `FATAL_ERROR` 跳过（Airymax 0.1.1 不允许桩库） |
| libmicrohttpd | `AGENTRT_HAS_MICROHTTPD` | HTTP 网关不可用 |
| libwebsockets | `AGENTRT_HAS_LIBWEBSOCKETS` | WebSocket 网关不可用 |
| libcurl | `AGENTRT_HAS_CURL` | 基准测试使用模拟模式 |

### 构建产物

- `gateway` —— 聚合所有网关组件的静态库
- 公共头文件安装到 `include/agentrt/gateway`

### 安装

```bash
cmake --install build --prefix /opt/airymax
```

### 配置示例

```json
{
  "gateway": {
    "http_port": 8080,
    "ws_port": 8081,
    "metrics_port": 9090,
    "tls_cert": "/etc/agentrt/certs/server.crt",
    "tls_key": "/etc/agentrt/certs/server.key",
    "rate_limit": {
      "enabled": true,
      "requests_per_second": 1000,
      "requests_per_minute": 50000,
      "burst_size": 2000
    },
    "cors": {
      "allowed_origins": ["*"],
      "allowed_methods": ["GET", "POST", "OPTIONS"]
    }
  }
}
```

---

## 6. 许可证

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.

本模块采用双许可证，您可以选择以下任一许可证遵守：

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt))，或
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

完整许可证文本见 [LICENSE](LICENSE) 文件，版权声明见 [NOTICE](NOTICE)。
默认适用 AGPL-3.0-or-later 条款；Apache-2.0 备选用于 AGPL 无法覆盖的
下游集成场景（如闭源或专有分发）。
