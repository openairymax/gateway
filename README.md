# gateway — HTTP/WS/Stdio → JSON-RPC 2.0 Gateway

> The external entry point into the Airymax runtime: every inbound HTTP, WebSocket, or Stdio request becomes a unified JSON-RPC 2.0 call.
> Leaf repository under the [agentrt](../) management repo.

**Language:** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.1-5a6b7e)](https://atomgit.com/openairymax/gateway)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)

- **Repository:** `git@atomgit.com:openairymax/gateway.git`
- **Branch:** `feature/official-hubs-01`
- **Version:** 0.1.1 (Airymax foundational release)

---

## Overview

**gateway** is the **protocol gateway layer** of the Airymax agent runtime. It is the bridge that connects the external world to the Airymax kernel: it takes inbound HTTP, WebSocket, and Stdio requests and uniformly translates them into internal JSON-RPC 2.0 calls that are dispatched through `agentrt/atoms/syscall` into the kernel services.

gateway follows the **K-1 kernel-minimalism** principle strictly: it does **only protocol translation, zero business logic**. Every business decision is delegated to the kernel via the syscall interface. It auto-detects and converts JSON-RPC, MCP, A2A, and OpenAI-API protocols at the boundary, manages long-lived connections and WebSocket sessions, enforces token-bucket rate limiting and request authentication (via `cupolas`), and is built event-driven on libmicrohttpd and libwebsockets for high concurrency.

```
External client → gateway_d → gateway → atoms/syscall → backend services
                   (daemon)   (lib)      (kernel)
```

Within the Airymax 0.1.1 release, the workspace is partitioned into **38 repositories** (1 umbrella + 5 management + 29 leaf + 3 top-level); `gateway` is one of the 7 leaf repositories aggregated by the [agentrt](../) management repo, forming the **Gateway Layer** in the cyclic architecture (above the Protocol Layer `protocols`, below the Service Layer `daemons`). The gateway library is wrapped by the `gateway_d` daemon (in `daemons/`) which exposes it as a system service.

## Module Classification

**Class — (Service / Composition layer).**

gateway is a service/composition module: it provides the external-facing protocol translation surface, not foundational primitives. It depends on `protocols` (router/gateway/registry interfaces, A2A/MCP/OpenAI adapters), `atoms` (CoreKern types and IPC primitives; `atoms/syscall` is the dispatch target; `atoms/memory`), `commons` (platform/types/error/logging/sync/memory macros), and logically on `cupolas` (request authentication and input sanitization at the protocol boundary). Its primary consumer is the `gateway_d` daemon, plus SDK/external clients connecting over HTTP/WS/Stdio/MCP.

## Directory Structure

```
gateway/
├── CMakeLists.txt                       # CMake build configuration (static lib gateway)
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
│       └── gateway_utils.h              # Generic helper macros & inlines
├── tests/                               # Tests & benchmarks
│   ├── test_gateway.c                   # Gateway main test (7 cases)
│   ├── test_jsonrpc.c                   # JSON-RPC protocol test (17 cases)
│   ├── test_syscall_router.c            # Syscall router test (8 cases)
│   ├── test_gateway_rpc_handler.c       # RPC handler test (14 cases)
│   └── gateway_benchmark.c              # Performance benchmark
├── deploy/                              # K8s deployment (namespace / configmap / deployment / service)
└── config/                              # Static-analysis config (cppcheck.cfg)
```

## Core Components

| Component | File | Responsibility |
|-----------|------|----------------|
| **Public API** | `include/gateway.h` | Unified lifecycle API: `create / start / stop / destroy` |
| **HTTP gateway** | `http_gateway.c` | libmicrohttpd-based HTTP server with dynamic endpoint registration |
| **WebSocket gateway** | `ws_gateway.c` | libwebsockets-based bidirectional RPC |
| **Stdio gateway** | `stdio_gateway.c` | Stdin/stdout REPL mode, blocking single-threaded |
| **Protocol bridge** | `gateway_protocol_bridge.c` | Gateway ↔ protocols module bridge with auto-detection |
| **JSON-RPC 2.0** | `jsonrpc.c` | Request validation, response generation, batch processing, notifications |
| **Syscall router** | `syscall_router.c` | JSON-RPC method-name → syscall function dispatch |
| **RPC handler** | `gateway_rpc_handler.c` | Shared RPC handling logic across all three gateways |
| **Multi-protocol handler** | `gateway_protocol_handler.c` | MCP / A2A / OpenAI auto-detection and conversion |
| **Rate limiter** | `gateway_rate_limiter.c` | Token-bucket algorithm, per-IP / per-API-Key throttling |

## Architecture

```
┌──────────────────────────────────────────────┐
│             Applications (OpenLab)            │
├──────────────────────────────────────────────┤
│             Ecosystem (Toolkit / SDK)         │
├──────────────────────────────────────────────┤
│              Daemon Services (daemons)        │
├──────────────────────────────────────────────┤
│          ★ gateway (Gateway Layer) ★         │
├──────────────────────────────────────────────┤
│   protocols / heapstore / cupolas             │
├──────────────────────────────────────────────┤
│            atoms / commons / OS               │
└──────────────────────────────────────────────┘

External client
  │
  ├─ HTTP REST ───→ http_gateway ──→ JSON-RPC 2.0 ──→ syscall_router ──→ backend
  ├─ WebSocket ──→ ws_gateway ────→ JSON-RPC 2.0 ──→ syscall_router ──→ backend
  ├─ Stdio ──────→ stdio_gateway ─→ JSON-RPC 2.0 ──→ syscall_router ──→ backend
  └─ MCP ────────→ gateway_protocol_handler → JSON-RPC 2.0 ──→ syscall_router ──→ backend
                         │
              gateway_protocol_handler  (auto-detect / convert / unified)
                         │
              gateway_rpc_handler       (shared RPC logic)
                         │
                  atoms/syscall → kernel services
```

**Design principles:** K-1 kernel-minimalism (protocol translation only, zero business logic); multi-protocol auto-detection (JSON-RPC / MCP / A2A / OpenAI-API); event-driven high concurrency (libmicrohttpd + libwebsockets); security at boundary (cupolas authentication + token-bucket rate limiting + CORS); dynamic endpoint extensibility.

## Upstream Dependencies

> `commons` is the foundation for all agentrt modules; gateway consumes it. gateway also depends on `protocols`, `atoms`, and logically `cupolas`.

| Dependency | Source | Purpose |
|------------|--------|---------|
| **protocols** | `agentrt/protocols/` | Protocol router / gateway / registry interfaces; A2A / MCP / OpenAI adapters — linked as `airy_protocols` |
| **atoms** | `agentrt/atoms/` | CoreKern types and IPC primitives; `atoms/syscall` is the dispatch target for `syscall_router`; `atoms/memory` linked as `airy_memory` |
| **commons** | `agentrt/commons/` | Platform abstraction, types, error framework, logging, sync, memory macros — linked as `airy_common` |
| **cupolas** | `agentrt/cupolas/` | Logical upstream: gateway invokes cupolas for request authentication and input sanitization at the protocol boundary |
| cJSON | external | JSON parsing — **hard dependency** as of Airymax 0.1.1 (stub library is not supported) |
| libmicrohttpd | external | HTTP server (≥ 0.9.70) |
| libwebsockets | external | WebSocket support (≥ 4.3.0) |
| OpenSSL | external | TLS/SSL (≥ 1.1.1) |
| libcurl | external | HTTP client for benchmarks (optional; benchmark falls back to simulation mode) |

## Downstream Consumers

| Consumer | What they use |
|----------|---------------|
| **gateway_d** | The gateway daemon (`agentrt/daemons/gateway_d/`) wraps this library and exposes it as a system service |
| SDK / external clients | SDK ships gateway client libraries; external clients connect over HTTP / WebSocket / Stdio / MCP |
| Agent applications | Agent apps invoke the runtime through the gateway's JSON-RPC 2.0 surface |

## Build

```bash
# Build the gateway module (requires cJSON dev headers — hard dependency)
cmake -S . -B /tmp/gateway-build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_BENCHMARK=ON
cmake --build /tmp/gateway-build --target gateway --parallel $(nproc)

# Run tests
ctest --test-dir /tmp/gateway-build -R "gateway|jsonrpc|syscall|rpc_handler" --output-on-failure

# Run performance benchmark
/tmp/gateway-build/gateway_benchmark

# Static analysis & formatting (if targets exist)
cmake --build /tmp/gateway-build --target cppcheck
cmake --build /tmp/gateway-build --target format

# Install
cmake --install /tmp/gateway-build --prefix /opt/airymax
```

**CMake options & conditional compilation:**

| Dependency | Conditional Macro | Behavior When Missing |
|------------|-------------------|-----------------------|
| cJSON | `AIRY_HAS_CJSON` | **Hard failure** — gateway module is skipped with `FATAL_ERROR` (Airymax 0.1.1 disallows stub libraries) |
| libmicrohttpd | `AIRY_HAS_MICROHTTPD` | HTTP gateway unavailable |
| libwebsockets | `AIRY_HAS_LIBWEBSOCKETS` | WebSocket gateway unavailable |
| libcurl | `AIRY_HAS_CURL` | Benchmark runs in simulation mode |

**Build artifacts:**

- `gateway` — static library aggregating all gateway components
- Public headers installed under `include/agentrt/gateway`

**Configuration example:**

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

## API

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

`GATEWAY_SUCCESS` (0), `GATEWAY_ERROR_INVALID` (-1), `GATEWAY_ERROR_MEMORY` (-2), `GATEWAY_ERROR_IO` (-3), `GATEWAY_ERROR_TIMEOUT` (-4), `GATEWAY_ERROR_CLOSED` (-5), `GATEWAY_ERROR_PROTOCOL` (-6).

### Usage example

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

## License

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.

This module is dual-licensed under the terms of either:

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt)), or
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

The full license texts are in the [LICENSE](LICENSE) file; the copyright notice is in [NOTICE](NOTICE). You may select either license to comply with. The AGPL-3.0-or-later terms apply by default; the Apache-2.0 alternative is provided for downstream integration scenarios (e.g., closed-source or proprietary distribution) that the AGPL does not accommodate.
