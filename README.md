**Language:** English | [简体中文](README_zh.md)

# Airymax Gateway — HTTP / gRPC Gateway Daemon

`agentrt/gateway/`

**Version:** 0.1.1
**License:** AGPL-3.0-or-later OR Apache-2.0 (dual-licensed)
**Branch:** `feature/official-hubs-01`

---

## 1. Module Positioning

Gateway is the **protocol gateway layer** of the Airymax agent runtime. It is
the bridge that connects the external world to the Airymax kernel: it takes
inbound HTTP, WebSocket, and Stdio requests and uniformly translates them into
internal JSON-RPC 2.0 calls that are dispatched through `agentrt/atoms/syscall`
into the kernel services.

Gateway follows the **K-1 kernel-minimalism** principle strictly: it does
**only protocol translation, zero business logic**. Every business decision is
delegated to the kernel via the syscall interface.

```
External client → gateway_d → gateway → atoms/syscall → backend services
                   (daemon)   (lib)      (kernel)
```

Design goals:

- **Protocol translation** — convert HTTP / WebSocket / Stdio requests into
  internal JSON-RPC 2.0 calls.
- **Multi-protocol support** — auto-detect and convert JSON-RPC, MCP, A2A, and
  OpenAI-API protocols.
- **Connection management** — manage client long-lived connections, WebSocket
  sessions, and concurrent requests.
- **Security enforcement** — token-bucket rate limiting, request
  authentication, CORS policies.
- **High concurrency** — event-driven architecture based on libmicrohttpd and
  libwebsockets.
- **Extensibility** — dynamic endpoint registration and custom request-handler
  injection.

---

## 2. Directory Structure

```
gateway/
├── CMakeLists.txt                       # CMake build configuration
├── README.md                            # This file (English)
├── README_zh.md                         # Chinese version
├── LICENSE                              # Dual license texts (AGPL-3.0 + Apache-2.0)
├── NOTICE                               # Copyright notice
├── include/                             # Public headers
│   ├── gateway.h                        # Unified public API (lifecycle / control / query)
│   └── gateway_protocol_bridge.h        # Gateway ↔ protocols bridge interface
├── src/
│   ├── gateway/                         # Core gateway implementation
│   │   ├── gateway.h                    # Internal header (ops vtable, structs)
│   │   ├── gateway_internal.h           # Internal types and function decls
│   │   ├── gateway_api.c                # Public API (create / start / stop / destroy)
│   │   ├── http_gateway.c/.h            # HTTP gateway (libmicrohttpd)
│   │   ├── http_gateway_routes.c/.h     # Static HTTP route table
│   │   ├── ws_gateway.c/.h              # WebSocket gateway (libwebsockets)
│   │   ├── stdio_gateway.c/.h           # Stdio gateway (REPL interaction)
│   │   └── gateway_protocol_bridge.c    # Protocol bridge impl
│   └── utils/                           # Utility modules
│       ├── jsonrpc.c/.h                 # JSON-RPC 2.0 utilities (validate / response / batch)
│       ├── syscall_router.c/.h          # JSON-RPC method → syscall dispatch
│       ├── gateway_rpc_handler.c/.h     # Shared RPC handling for HTTP/WS/Stdio
│       ├── gateway_protocol_handler.c/.h  # MCP / A2A / OpenAI auto-detection & conversion
│       ├── gateway_rate_limiter.c/.h    # Token-bucket rate limiter
│       ├── gateway_utils.h              # Generic helper macros & inlines
│       ├── gateway_compat.h             # Compatibility definitions
│       └── mcp_server.c/.h              # MCP protocol server (tools / resources / prompts)
├── tests/                               # Tests & benchmarks
│   ├── test_gateway.c                   # Gateway main test (7 cases)
│   ├── test_jsonrpc.c                   # JSON-RPC protocol test (17 cases)
│   ├── test_syscall_router.c            # Syscall router test (8 cases)
│   ├── test_gateway_rpc_handler.c       # RPC handler test (14 cases)
│   └── gateway_benchmark.c              # Performance benchmark
├── docker/                              # Docker deployment (Dockerfile, compose files, monitoring)
├── deploy/                              # K8s deployment (namespace / configmap / deployment / service)
└── config/                              # Static-analysis config (cppcheck.cfg)
```

### Core Components

| Component | File | Responsibility |
|-----------|------|----------------|
| **Public API** | `include/gateway.h` | Unified lifecycle API: `create / start / stop / destroy` |
| **HTTP gateway** | `http_gateway.c` | libmicrohttpd-based HTTP server with dynamic endpoint registration |
| **WebSocket gateway** | `ws_gateway.c` | libwebsockets-based bidirectional RPC |
| **Stdio gateway** | `stdio_gateway.c` | Stdin/stdout REPL mode, blocking single-threaded |
| **Protocol bridge** | `gateway_protocol_bridge.c` | Gateway ↔ protocols module bridge with auto-detection |

### Utility Modules

| Component | File | Responsibility |
|-----------|------|----------------|
| **JSON-RPC 2.0** | `jsonrpc.c` | Request validation, response generation, batch processing, notifications |
| **Syscall router** | `syscall_router.c` | JSON-RPC method-name → syscall function dispatch |
| **RPC handler** | `gateway_rpc_handler.c` | Shared RPC handling logic across all three gateways |
| **Multi-protocol handler** | `gateway_protocol_handler.c` | MCP / A2A / OpenAI auto-detection and conversion |
| **Rate limiter** | `gateway_rate_limiter.c` | Token-bucket algorithm, per-IP / per-API-Key throttling |
| **MCP server** | `mcp_server.c` | MCP server with tool / resource / prompt registration |

### Architecture

```
External client
  │
  ├─ HTTP REST ───→ http_gateway ──→ JSON-RPC 2.0 ──→ syscall_router ──→ backend
  ├─ WebSocket ──→ ws_gateway ────→ JSON-RPC 2.0 ──→ syscall_router ──→ backend
  ├─ Stdio ──────→ stdio_gateway ─→ JSON-RPC 2.0 ──→ syscall_router ──→ backend
  └─ MCP ────────→ mcp_server ────→ JSON-RPC 2.0 ──→ syscall_router ──→ backend
                         │
              gateway_protocol_handler  (auto-detect / convert / unified)
                         │
              gateway_rpc_handler       (shared RPC logic)
                         │
                  IPC service bus
```

---

## 3. Upstream / Downstream Dependencies

### Upstream (Gateway depends on)

| Dependency | Source | Purpose |
|------------|--------|---------|
| **protocols** | `agentrt/protocols/` | Protocol router / gateway / registry interfaces; A2A / MCP / OpenAI adapters — linked as `agentrt_protocols` |
| **atoms** | `agentrt/atoms/` | CoreKern types and IPC primitives; `atoms/syscall` is the dispatch target for `syscall_router`; `atoms/memory` linked as `agentrt_memory` |
| **commons** | `agentrt/commons/` | Platform abstraction, types, error framework, logging, sync, memory macros — linked as `agentrt_common` |
| **cupolas** | `agentrt/cupolas/` | Logical upstream: gateway invokes Cupolas for request authentication and input sanitization at the protocol boundary |
| cJSON | external | JSON parsing — **hard dependency** as of Airymax 0.1.1 (stub library is not supported) |
| libmicrohttpd | external | HTTP server (≥ 0.9.70) |
| libwebsockets | external | WebSocket support (≥ 4.3.0) |
| OpenSSL | external | TLS/SSL (≥ 1.1.1) |
| libcurl | external | HTTP client for benchmarks (optional; benchmark falls back to simulation mode) |

### Downstream (consumers of Gateway)

| Consumer | What it uses |
|----------|--------------|
| **gateway_d** | The gateway daemon (`agentrt/daemons/gateway_d/`) wraps this library and exposes it as a system service |
| SDK / external clients | SDK ships gateway client libraries; external clients connect over HTTP / WebSocket / Stdio / MCP |
| Agent applications | Agent apps invoke the runtime through the gateway's JSON-RPC 2.0 surface |

---

## 4. Public API

### Lifecycle

| Function | Description |
|----------|-------------|
| `gateway_http_create(host, port)` | Create an HTTP gateway instance |
| `gateway_ws_create(host, port)` | Create a WebSocket gateway instance |
| `gateway_stdio_create()` | Create a Stdio gateway instance |
| `gateway_destroy(gw)` | Destroy a gateway instance and release resources |
| `gateway_start(gw)` | Start the gateway (HTTP/WS non-blocking; Stdio blocking) |
| `gateway_stop(gw)` | Gracefully stop the gateway |

### Control & Query

| Function | Description |
|----------|-------------|
| `gateway_set_handler(gw, handler, user_data)` | Set a custom request handler callback |
| `gateway_register_endpoint(gw, method, path, handler, user_data)` | Register a dynamic HTTP endpoint |
| `gateway_get_type(gw)` | Get the gateway type enum |
| `gateway_is_running(gw)` | Check whether the gateway is running |
| `gateway_get_stats(gw, out_json)` | Get JSON-format statistics |
| `gateway_get_name(gw)` | Get the gateway name |

### Error codes

`GATEWAY_SUCCESS` (0), `GATEWAY_ERROR_INVALID` (-1), `GATEWAY_ERROR_MEMORY` (-2),
`GATEWAY_ERROR_IO` (-3), `GATEWAY_ERROR_TIMEOUT` (-4), `GATEWAY_ERROR_CLOSED` (-5),
`GATEWAY_ERROR_PROTOCOL` (-6).

### Usage Example

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

## 5. Build Instructions

```bash
# Build the gateway module (requires cJSON dev headers)
cmake -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARK=ON
cmake --build build --target gateway

# Run tests
ctest --test-dir build -R "gateway|jsonrpc|syscall|rpc_handler"

# Run performance benchmark
./build/gateway_benchmark

# Static analysis & formatting (if targets exist)
cmake --build build --target cppcheck
cmake --build build --target format
```

### CMake Options & Conditional Compilation

| Dependency | Conditional Macro | Behavior When Missing |
|------------|-------------------|-----------------------|
| cJSON | `AGENTRT_HAS_CJSON` | **Hard failure** — gateway module is skipped with `FATAL_ERROR` (Airymax 0.1.1 disallows stub libraries) |
| libmicrohttpd | `AGENTRT_HAS_MICROHTTPD` | HTTP gateway unavailable |
| libwebsockets | `AGENTRT_HAS_LIBWEBSOCKETS` | WebSocket gateway unavailable |
| libcurl | `AGENTRT_HAS_CURL` | Benchmark runs in simulation mode |

### Build Artifacts

- `gateway` — static library aggregating all gateway components
- Public headers installed under `include/agentrt/gateway`

### Installation

```bash
cmake --install build --prefix /opt/airymax
```

### Configuration Example

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

## 6. License

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.

This module is dual-licensed under the terms of either:

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt)), or
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

The full license texts are in the [LICENSE](LICENSE) file; the copyright
notice is in [NOTICE](NOTICE). You may select either license to comply with.
The AGPL-3.0-or-later terms apply by default; the Apache-2.0 alternative is
provided for downstream integration scenarios (e.g., closed-source or
proprietary distribution) that the AGPL does not accommodate.
