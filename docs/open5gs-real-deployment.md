# Open5GS 真实卫星部署（异步编排）说明

本文档对应当前代码实现：
- 前端点击“确认部署”后立即显示部署完成。
- 后端异步编排器在后台推进容器启动、NF 启动与服务就绪状态。
- `/api/v1/deployments`、`/api/v1/satellites` 返回实时运行态字段。
- WebSocket `deployment_runtime_update` 实时推送部署运行状态。

## 1. 部署运行态字段

`GET /api/v1/deployments` 目前包含以下关键字段：
- `orchestration_phase`: `queued/stopping_old/starting_containers/running/degraded/failed`
- `orchestration_progress`: 0-100
- `containers_total/running/failed`
- `core_nfs_total/running/failed`
- `service_ready`
- `ready_for_ueransim`
- `last_error`
- `last_update_at`

`GET /api/v1/satellites` 目前包含以下关键字段：
- `container_name`
- `container_state`: `stopped/starting/running/failed`
- `running_core_nf_types`
- `running_core_nf_count`
- `service_probe_ok`
- `core_business_load`
- `core_network_load`

未部署卫星节点会返回 0 业务负载与 `container_state=stopped`。

## 2. Open5GS 卫星容器镜像

默认镜像：`ghcr.io/open5gs/open5gs:latest`。

可按架构显式指定：
- `SFC_SATELLITE_IMAGE_ARM64`
- `SFC_SATELLITE_IMAGE_AMD64`

也可统一覆盖：
- `SFC_SATELLITE_IMAGE`

可选平台参数：
- `SFC_SATELLITE_PLATFORM`（例如 `linux/amd64`）

### 多架构构建（buildx）

仓库提供：
- Dockerfile: `docker/open5gs-satellite.Dockerfile`
- 脚本: `scripts/build_open5gs_multiarch.sh`

示例：

```bash
IMAGE_REPO=ghcr.io/<your-org>/sfc-open5gs-satellite \
IMAGE_TAG=v0.1.0 \
./scripts/build_open5gs_multiarch.sh
```

运行时建议环境变量：

```bash
export SFC_SATELLITE_IMAGE_ARM64=ghcr.io/<your-org>/sfc-open5gs-satellite:v0.1.0
export SFC_SATELLITE_IMAGE_AMD64=ghcr.io/<your-org>/sfc-open5gs-satellite:v0.1.0
```

## 3. 手动 UERANSIM 冒烟验证

脚本：`scripts/verify_ueransim_smoke.sh`

该脚本会：
1. 查询 `ready_for_ueransim=true` 的部署。
2. 自动定位运行 AMF 的卫星容器。
3. 启动 UERANSIM gNB/UE 容器并执行基础注册与最小业务验证。

示例：

```bash
./scripts/verify_ueransim_smoke.sh
```

常用可选参数：

```bash
API_BASE=http://127.0.0.1:8080/api/v1 \
SFC_OPEN5GS_NETWORK=sfc-open5gs-net \
UERANSIM_IMAGE=ghcr.io/herlesupreeth/docker_ueransim:latest \
VERIFY_TIMEOUT_SEC=120 \
./scripts/verify_ueransim_smoke.sh
```

## 4. 银河麒麟 V10(x86) 迁移清单

### 4.1 依赖
- Docker Engine + Docker Compose Plugin
- `curl`, `jq`, `bash`
- 可访问镜像仓库（Open5GS/UERANSIM/业务镜像）

### 4.2 内核与网络建议

```bash
sudo modprobe br_netfilter
sudo sysctl -w net.ipv4.ip_forward=1
sudo sysctl -w net.bridge.bridge-nf-call-iptables=1
sudo sysctl -w net.bridge.bridge-nf-call-ip6tables=1
```

### 4.3 端口与网络
- 后端 API: `8080`
- 前端开发端口按本地配置
- Open5GS/UERANSIM 使用 Docker 网络互联（默认 `sfc-open5gs-net`）

### 4.4 迁移步骤
1. 在麒麟 x86 拉取或构建 `linux/amd64` 卫星镜像。
2. 设置 `SFC_SATELLITE_IMAGE_AMD64`（或统一 `SFC_SATELLITE_IMAGE`）。
3. 启动后端/前端。
4. 生成策略并确认部署（前端立即完成，后台编排推进）。
5. 观察控制中心运行态字段。
6. 在 `ready_for_ueransim=true` 后执行 `scripts/verify_ueransim_smoke.sh`。

## 5. 当前实现边界
- 系统内不自动启动 UE（按需求保留手动验证脚本）。
- CPU/MEM/DISK 仍是模拟波动。
- 核心网业务负载来自“容器内真实运行 NF + 健康状态”感知结果。
