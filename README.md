# gateway — HTTP/WS/SSE/Stdio → JSON-RPC 2.0 protocol gateway

> The external entry point into the Airymax runtime: every inbound HTTP, HTTP/2, WebSocket, SSE, Stdio, MCP, A2A, or OpenAI-compatible request is translated into a unified JSON-RPC 2.0 call and dispatched to the runtime.

**Language:** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/gateway)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)

- **Repository:** <https://atomgit.com/openairymax/gateway>
- **Version:** 0.1.15
- **Artifact:** static library `gateway`

---

## What it is

**gateway** is the protocol gateway layer of the Airymax agent runtime. It sits between external clients and the runtime core: it accepts inbound HTTP, HTTP/2, WebSocket, SSE, and Stdio traffic, auto-detects the wire protocol (JSON-RPC, MCP, A2A, OpenAI-compatible), normalizes it into JSON-RPC 2.0, and forwards it inward — either through the syscall interface (`atoms/syscall`) or through a thin business-delegation layer that calls backend service endpoints (LLM, tool-execution, scheduling).

Its focus is protocol translation and connection management, not application logic: business decisions are handed off to the runtime. The module additionally provides cross-cutting boundary controls — token-bucket rate limiting, entry authentication (fail-closed, loopback-aware), URL sanitization, and CORS.

```
external client ──► gateway ──► JSON-RPC 2.0 ──► runtime (syscall / backend services)
                 (HTTP/HTTP2/WS/SSE/Stdio · MCP/A2A/OpenAI)
```

The `gateway` library is intended to be embedded and launched by a host process (for example a gateway daemon) that owns the event loop and graceful-shutdown lifecycle.

## Capabilities

| Area | Detail |
|------|--------|
| Transports | HTTP/1.1 (libmicrohttpd), HTTP/2 (nghttp2, POSIX only), WebSocket (libwebsockets), SSE, Stdio REPL |
| Protocol handling | Auto-detect and convert JSON-RPC 2.0, MCP, A2A, and OpenAI-compatible request bodies to a single internal JSON-RPC 2.0 shape |
| Inward dispatch | Route JSON-RPC methods to the syscall interface, or forward business calls (`agent.run`, `tools/call`, embeddings, task scheduling) to backend service endpoints |
| Streaming | Server-Sent Events for chat stream, agent run stream, and hall event watch |
| Boundary security | Entry authentication (fail-closed, loopback-aware), token-bucket rate limiting (per IP / per API key), URL sanitization, CORS |
| Operations | `/health`, `/metrics`, and JSON statistics snapshot via `gateway_get_stats()` |

## Composition

The library target `gateway` is built from three source areas:

| Area | Path | Responsibility |
|------|------|----------------|
| Transports | `src/gateway/` | HTTP, HTTP/2, WebSocket, SSE, Stdio servers; endpoint authentication; protocol bridge; hall event read/write |
| Business delegation | `src/biz/` | Unified protocol entry, `agent.run`/`tools/call`/embeddings/scheduling forwarding, capability registry, PEP cache |
| Shared utilities | `src/utils/` | JSON-RPC 2.0 helpers, syscall router, RPC handler, protocol detect/convert, rate limiter |

The same source list is shared with the gateway daemon host via `cmake/gateway-sources.cmake`.

### Directory structure

```
gateway/
├── CMakeLists.txt                       # CMake build (static library `gateway`)
├── cmake/
│   └── gateway-sources.cmake            # Source list shared with the daemon host
├── include/
│   ├── gateway.h                        # Public API (lifecycle / control / query)
│   └── gateway_protocol_bridge.h        # Gateway ↔ protocols bridge interface
├── src/
│   ├── gateway/                         # Transport layer
│   │   ├── gateway_api.c                # Public API implementation
│   │   ├── http_gateway.[ch]            # HTTP/1.1 server (libmicrohttpd)
│   │   ├── http_gateway_routes.[ch]     # Static HTTP route table + entry auth gate
│   │   ├── http_gateway_sse*.c          # SSE base + frame/stream/run/hall/memory/tool
│   │   ├── http2_gateway*.[ch]          # HTTP/2 server (nghttp2, POSIX only)
│   │   ├── ws_gateway*.[ch]             # WebSocket server (libwebsockets)
│   │   ├── stdio_gateway.[ch]           # Stdio REPL gateway
│   │   ├── gateway_auth.[ch]            # Entry authentication
│   │   ├── gateway_protocol_bridge.c    # Protocol bridge implementation
│   │   └── gateway_hall_store.[ch]      # Hall event recording (write side)
│   ├── biz/                             # Business-delegation layer
│   │   ├── gateway_business_handler.[ch]# Unified protocol entry + business chain
│   │   ├── gateway_biz_*.c              # forward / svcdispatch / hall / backend / tools / agent
│   │   ├── gateway_cap_registry.[ch]    # Capability registry
│   │   ├── gateway_pep_cache.[ch]       # PEP cache
│   │   └── gateway_svc_adapter.c        # Service adapter
│   └── utils/                           # Shared utilities
│       ├── jsonrpc.[ch]                 # JSON-RPC 2.0 validate / respond / batch
│       ├── syscall/                     # JSON-RPC method → syscall dispatch
│       ├── gateway_rpc_handler.[ch]     # Shared RPC handling for all transports
│       ├── gateway_protocol_*.[ch]      # Protocol detect / convert / handler
│       └── gateway_rate_limiter.[ch]    # Token-bucket rate limiter
├── tests/                               # Unit tests + benchmark
├── deploy/                              # Kubernetes manifests (see deploy/README.md)
└── config/                              # Static-analysis config (see config/README.md)
```

### HTTP routes

The static route table (see `src/gateway/http_gateway_routes.c`):

| Method | Path | Streaming | Auth | Purpose |
|--------|------|-----------|------|---------|
| `POST` | `/` | no | yes | JSON-RPC 2.0 entry (also MCP / A2A / OpenAI bodies) |
| `POST` | `/api/v1/chat/stream` | SSE | yes | Chat stream |
| `POST` | `/api/v1/agent/run/stream` | SSE | yes | Agent run stream |
| `GET` | `/api/v1/hall/watch` | SSE | no | Hall event watch |
| `OPTIONS` | `*` | no | no | CORS preflight |
| `GET` | `/health` | no | no | Liveness / readiness |
| `GET` | `/metrics` | no | yes | Metrics export |

## Usage

### Public API

Lifecycle and control (see `include/gateway.h`):

| Function | Description |
|----------|-------------|
| `gateway_http_create(host, port)` | Create an HTTP gateway instance |
| `gateway_ws_create(host, port)` | Create a WebSocket gateway instance |
| `gateway_stdio_create()` | Create a Stdio gateway instance (blocking REPL after start) |
| `gateway_start(gw)` | Start the gateway (HTTP/WS non-blocking; Stdio blocking) |
| `gateway_stop(gw)` | Gracefully stop the gateway |
| `gateway_destroy(gw)` | Destroy the instance and release resources |
| `gateway_set_handler(gw, handler, user_data)` | Install a custom request-handler callback |
| `gateway_register_endpoint(gw, method, path, handler, user_data)` | Register a dynamic HTTP endpoint |
| `gateway_get_type(gw)` | Get the gateway type (`HTTP` / `WS` / `STDIO`) |
| `gateway_is_running(gw)` | Check whether the gateway is running |
| `gateway_get_stats(gw, out_json)` | Get a JSON statistics snapshot (caller frees) |
| `gateway_get_name(gw)` | Get the gateway name |

```c
#include "gateway.h"

int main(void) {
    gateway_t *gw = gateway_http_create("0.0.0.0", 8080);
    if (!gw) return 1;

    gateway_start(gw);

    char *stats = NULL;
    gateway_get_stats(gw, &stats);   /* caller frees */
    printf("Stats: %s\n", stats);
    free(stats);

    gateway_stop(gw);
    gateway_destroy(gw);
    return 0;
}
```

### Error codes

`GATEWAY_SUCCESS` (0), `GATEWAY_ERROR_INVALID` (-1), `GATEWAY_ERROR_MEMORY` (-2), `GATEWAY_ERROR_IO` (-3), `GATEWAY_ERROR_TIMEOUT` (-4), `GATEWAY_ERROR_CLOSED` (-5), `GATEWAY_ERROR_PROTOCOL` (-6).

Boundary rejections are reported as JSON-RPC errors: authentication failure (`-32001`), unsafe URL (`-32002`), rate limit exceeded (`-32004`), parse error (`-32700`), method not found (`-32601`), oversized request (`413`), internal error (`-32603`).

## Build

```bash
# Requires cJSON development headers (hard dependency).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_BENCHMARK=ON
cmake --build build --target gateway --parallel

# Tests
ctest --test-dir build --output-on-failure

# Static analysis & formatting
cmake --build build --target cppcheck
cmake --build build --target format
```

**CMake options:**

| Option | Default | Effect |
|--------|---------|--------|
| `BUILD_TESTS` | `ON` | Build the unit-test targets (disabled on Windows) |
| `BUILD_BENCHMARK` | `ON` | Build the benchmark target (Linux only) |

**Conditional compilation:**

| Feature | Macro | Behavior when absent |
|---------|-------|----------------------|
| cJSON | `AIRY_HAS_CJSON` | **Hard failure** — `FATAL_ERROR` (stub libraries are not supported) |
| HTTP/1.1 | `AIRY_HAS_MICROHTTPD` → `GATEWAY_HAS_HTTP` | HTTP gateway unavailable |
| WebSocket | `AIRY_HAS_LIBWEBSOCKETS` | WebSocket gateway unavailable |
| HTTP/2 | `AIRY_HAS_HTTP2` | HTTP/2 gateway unavailable; **always disabled on Windows (POSIX only)** |
| libcurl | `AIRY_HAS_CURL` | Benchmark runs in simulation mode |

**Artifact:** static library `gateway`; public headers install under `include/agentrt/gateway`.

### Runtime configuration

The gateway is configured through environment variables (there is no JSON config file).

| Variable | Purpose |
|----------|---------|
| `GATEWAY_API_KEY` | API key for entry authentication |
| `GATEWAY_RATE_LIMIT_ENABLED` / `_RPS` / `_RPM` | Enable rate limiting; requests per second / minute |
| `GATEWAY_CORS_MODE` / `GATEWAY_CORS_ORIGINS` | CORS policy |
| `GATEWAY_HTTP_CONN_LIMIT` / `GATEWAY_HTTP_TIMEOUT` / `GATEWAY_HTTP_THREADS` | HTTP/1.1 server tuning |
| `GATEWAY_HTTP2_MAX_STREAMS` / `GATEWAY_HTTP2_TIMEOUT` | HTTP/2 server tuning |
| `GATEWAY_MAX_REQUEST_SIZE` | Maximum request body size |
| `GATEWAY_PROTOCOL_HANDLER` | Protocol-handler selection |
| `AIRY_LLM_SOCK` / `AIRY_LLM_TCP_ADDR` / `AIRY_LLM_TCP_PORT` | LLM backend endpoint |
| `AIRY_AGENT_SOCK` / `AIRY_TOOL_SOCK` | Agent / tool backend endpoints |
| `AIRY_AGENT_MODEL` | Default model for agent runs |
| `AIRY_GW_SSE_MAX_TOKENS` / `AIRY_GW_SSE_MAX_TOOL_LOOPS` | SSE stream limits |
| `AIRY_MAX_SESSIONS` / `AIRY_RATE_LIMIT_TABLE_SIZE` | Session and rate-limiter table sizes |
| `AIRY_STDIO_BUFFER_SIZE` / `AIRY_GATEWAY_MEM_PUBLIC` | Stdio buffer size; public-memory flag |

## Relationships

**Upstream dependencies** (linked by this module):

| Dependency | Provides |
|------------|----------|
| [protocols](https://atomgit.com/openairymax/protocols) | Router / gateway / registry interfaces and the A2A / MCP / OpenAI adapters (`airy_protocols`) |
| [agentrt atoms](https://atomgit.com/openairymax/atoms) | Syscall dispatch target and memory primitives (`airy_memory`) |
| [agentrt commons](https://atomgit.com/openairymax/commons) | Platform, types, error, logging, and sync foundation (`airy_common`, `svc_common`, `airy_syscall`) |
| cJSON (external) | JSON parsing — **hard dependency** |
| libmicrohttpd / libwebsockets / nghttp2 / OpenSSL | HTTP, WebSocket, HTTP/2, and TLS transports |

**Downstream consumers:** the gateway daemon host wraps this library and runs it as a system service; SDKs and agent applications connect over HTTP / WebSocket / SSE / Stdio / MCP.

## License

Copyright (c) 2025-2026 SPHARX Ltd.

This module is open source and dual-licensed; you may choose either license:

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt)), or
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

The full license texts are in [LICENSE](LICENSE); the copyright notice is in [NOTICE](NOTICE).
