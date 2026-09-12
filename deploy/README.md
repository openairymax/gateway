# Gateway Deploy — Kubernetes 部署配置

**位置：** `deploy/k8s/`
**版本：** 0.1.15

## 概述

`gateway/deploy/` 提供 AirymaxRT 网关的 Kubernetes 部署清单，覆盖命名空间、多环境配置、滚动更新、自动扩缩容、服务暴露、Ingress TLS 与网络策略。清单以变量占位（镜像地址、TLS 域名）便于适配不同集群，可直接 `kubectl apply`，也可作为 GitOps 基线。

## 目录结构

```
deploy/
├── k8s/
│   ├── namespace.yaml      # 命名空间（agentrt-gateway）
│   ├── configmap.yaml      # 三环境 ConfigMap（development / staging / production）
│   ├── deployment.yaml     # Deployment + ServiceAccount + HorizontalPodAutoscaler
│   ├── service.yaml        # ClusterIP + Headless Service + Ingress + NetworkPolicy
│   └── secret.yaml         # Secret 模板（gateway_api_key / jwt_secret）
└── README.md               # 本文件
```

## 资源清单

### namespace.yaml

创建 `agentrt-gateway` 命名空间，隔离网关资源。

### configmap.yaml

按环境划分三份同名 ConfigMap `agentrt-gateway-manager`，分别位于 `agentrt-development` / `agentrt-staging` / `agentrt-production`：

| 键 | Development | Staging | Production |
|----|-------------|---------|------------|
| `AGENTRT_ENV` | development | staging | production |
| `AGENTRT_LOG_LEVEL` | DEBUG | INFO | WARN |
| `AGENTRT_MAX_SESSIONS` | 100 | 500 | 1000 |
| `AGENTRT_SESSION_TIMEOUT` | 3600 | 1800 | 900 |

三份共用的键：`AGENTRT_HTTP_PORT=8080`、`AGENTRT_WS_PORT=8081`、`AGENTRT_HEALTH_CHECK_INTERVAL=30`、`REDIS_PORT=6379`、`ENABLE_METRICS=true`、`ENABLE_TRACING=true`，以及按环境区分的 `REDIS_HOST`。

### deployment.yaml

| 项 | 值 |
|----|----|
| **Deployment** | 3 副本，RollingUpdate（`maxSurge=1`、`maxUnavailable=0`） |
| **镜像** | `${GATEWAY_IMAGE:-ghcr.io/spharx/agentrt-gateway}:latest` |
| **端口** | `http` 8080 / `websocket` 8081 / `metrics` 9090 |
| **ServiceAccount** | `agentrt-gateway` |
| **安全上下文** | `runAsNonRoot`，UID/GID/fsGroup `1000` |
| **调度** | Pod 反亲和（preferred，按主机名拓扑分散）；容忍 `not-ready` / `unreachable` |
| **卷挂载** | `/etc/agentrt/gateway`（ConfigMap，只读）、`/var/log/agentrt`（emptyDir） |
| **HPA** | `autoscaling/v2`，3–10 副本，CPU 70% / 内存 80%；缩容稳定窗口 300s |

### service.yaml

| 资源 | 说明 |
|------|------|
| **ClusterIP** | `agentrt-gateway`，暴露 HTTP 8080 / WebSocket 8081 / Metrics 9090 |
| **Headless** | `agentrt-gateway-headless`（`clusterIP: None`），用于 Pod 直接寻址 |
| **Ingress** | Nginx Ingress + cert-manager（`letsencrypt-prod`），API 与 WebSocket 双域名，TLS 终止，`proxy-body-size: 10m` |
| **NetworkPolicy** | 入站仅放行 Ingress 命名空间（8080/8081）与监控命名空间（9090）；出站限 DNS（53）、Redis（6379）、外部 HTTPS（443） |

Ingress 域名以变量占位：`${GATEWAY_API_HOST:-api.agentrt.example.com}`、`${GATEWAY_WS_HOST:-ws.agentrt.example.com}`，证书 secret 为 `agentrt-gateway-tls`。

### secret.yaml

`agentrt-gateway-secrets`（Opaque）为**模板**，占位值不可直接用于生产。真实凭证请用命令行生成，避免明文入库：

```bash
kubectl -n agentrt-gateway create secret generic agentrt-gateway-secrets \
  --from-literal=gateway_api_key="$(openssl rand -hex 32)" \
  --from-literal=jwt_secret="$(openssl rand -hex 32)"
```

`gateway_api_key` 为网关入口凭证，客户端以 `Authorization: Bearer <key>` 携带。Kubernetes 形态必须注入该值：未配置时网关以 fail-closed 方式将监听收敛到 `127.0.0.1`，经 Ingress 转发的流量将不可达。

## 容器环境变量

Deployment 注入下列环境变量（值来自 ConfigMap / Secret）：

| 变量 | 来源 | 说明 |
|------|------|------|
| `AGENTRT_MODULE` | 常量 `gateway` | 模块标识 |
| `AGENTRT_LOG_LEVEL` | ConfigMap | 日志级别 |
| `AGENTRT_HTTP_PORT` / `AGENTRT_WS_PORT` | 常量 | 监听端口 8080 / 8081 |
| `AGENTRT_METRICS_PORT` | 常量 | 指标端口 9090 |
| `AGENTRT_REDIS_HOST` / `AGENTRT_REDIS_PORT` | ConfigMap | Redis 端点 |
| `AGENTRT_JWT_SECRET` | Secret `jwt_secret` | JWT 签名密钥 |
| `GATEWAY_API_KEY` | Secret `gateway_api_key` | 入口鉴权凭证 |
| `AGENTRT_ENABLE_AUTH` | ConfigMap | 是否启用鉴权 |

> 网关库在进程内另以 `GATEWAY_*` / `AIRY_*` 环境变量细化传输、限流与后端端点，完整清单见 [../README.md](../README.md) 的「运行时配置」。

## 健康检查与资源

| 探针 | 路径 | 初始延迟 | 周期 | 超时 | 失败阈值 |
|------|------|---------|------|------|---------|
| **liveness** | `/health` | 10s | 15s | 5s | 3 |
| **readiness** | `/health/ready` | 5s | 10s | 3s | 3 |
| **startup** | `/health` | 0s | 5s | 3s | 30 |

| 资源 | Requests | Limits |
|------|----------|--------|
| CPU | 500m | 2 |
| Memory | 128Mi | 512Mi |

## 部署步骤

```bash
# 1. 命名空间
kubectl apply -f k8s/namespace.yaml

# 2. 配置（按目标环境选用对应 ConfigMap）
kubectl apply -f k8s/configmap.yaml

# 3. 创建入口凭证 Secret（见上文 secret.yaml 说明）

# 4. 工作负载（Deployment + ServiceAccount + HPA）
kubectl apply -f k8s/deployment.yaml

# 5. 暴露（Service + Headless + Ingress + NetworkPolicy）
kubectl apply -f k8s/service.yaml
```

## 监控

Pod 模板带有 Prometheus 抓取注解 `prometheus.io/scrape=true`、`prometheus.io/port=9090`、`prometheus.io/path=/metrics`，可被集群内 Prometheus 自动发现并采集 `/metrics`。

## 依赖

| 组件 | 用途 |
|------|------|
| Kubernetes ≥ 1.24 | 容器编排 |
| cert-manager | Ingress TLS 证书签发 |
| Nginx Ingress Controller | 七层路由与 WebSocket 转发 |
| Prometheus | 指标采集（可选） |

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

本目录随网关模块一同发布，采用双许可证：`AGPL-3.0-or-later OR Apache-2.0`。完整文本见仓库根的 [LICENSE](../LICENSE)。
