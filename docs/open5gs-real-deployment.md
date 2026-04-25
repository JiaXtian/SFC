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

当执行 `SFC 回滚/删除` 时，系统会：
- 停止并删除该部署关联的卫星容器
- 清空对应卫星节点的运行网元信息
- 将对应卫星节点 `core_business_load/core_network_load` 复位为 0

### 1.1 卫星容器命名规则

每个卫星容器名称包含 **SFC 部署标识** 与 **卫星节点标识**：

`sfc-sat-<deployment_id>-<satellite_node_id>`

例如：

`sfc-sat-deploy-1777040402348-sat-000-003`

说明：
- `<deployment_id>` 来自部署记录（如 `deploy_1777040402348`）
- `<satellite_node_id>` 来自节点 ID（如 `SAT_000_003`）
- 重调度沿用同一部署 ID，容器会按“先停后起”在该命名空间下重建

## 2. Open5GS 卫星容器镜像

### 2.1 当前 SFC 模板与网元集合

系统策略模板已收敛为 1 个完整链路模板：
- `SA-Full-12`
- 网元顺序：`nrf, ausf, udm, udr, amf, smf, upf, pcf, nssf, scp, bsf, sepp`

同时支持“自定义 SFC”：
- 可按需增删/调整网元参数
- 管理员可配置“同星绑定组”，将指定网元强制部署到同一颗卫星
- 前端会提示是否缺少基础可服务网元与是否缺少完整 12 网元

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
4. 自动处理 API 鉴权（支持显式 `API_TOKEN`，或用 `API_USERNAME/API_PASSWORD` 自动登录）。

示例：

```bash
./scripts/verify_ueransim_smoke.sh
```

常用可选参数：

```bash
API_BASE=http://127.0.0.1:18080/api/v1 \
API_TOKEN=<jwt_token> \
# 或使用账号自动登录（默认 admin / 123456）
# API_USERNAME=admin API_PASSWORD=123456 \
STRICT_PDU_SESSION=1 \
PDU_WAIT_SEC=30 \
SFC_OPEN5GS_NETWORK=sfc-open5gs-net \
UERANSIM_IMAGE=docker.io/free5gc/ueransim:latest \
DEPLOYMENT_ID=deploy_xxx \
VERIFY_TIMEOUT_SEC=120 \
./scripts/verify_ueransim_smoke.sh
```

说明：
- 默认镜像已切换为 Docker Hub：`docker.io/free5gc/ueransim:latest`
- 脚本会自动 `docker pull` 指定镜像
- 未显式设置 `API_BASE` 时，脚本会自动探测 `http://127.0.0.1:18080/api/v1` 与 `http://127.0.0.1:8080/api/v1`
- 可通过 `DEPLOYMENT_ID` 精确验证指定部署；不传时默认选择最新 `ready_for_ueransim=true` 的部署
- 脚本默认仅验证 `ready_for_ueransim=true` 的部署；如需强制对 `service_ready=true` 但未标记 UE 就绪的部署执行测试，可设置 `ALLOW_SERVICE_READY_FALLBACK=1`
- 默认切片参数：`SST=1`、`SD=000001`（可通过 `UERANSIM_SST/UERANSIM_SD` 覆盖）
- 脚本会自动向 `sfc-open5gs-mongo` 写入/更新测试订阅数据（IMSI/KEY/OPC/APN）
- 脚本会先强制验证注册成功；可通过 `STRICT_PDU_SESSION=1` 强制要求在 `PDU_WAIT_SEC` 内出现 PDU 会话建立成功日志。
- 若日志出现 `TUN allocation failure [Open failure /dev/net/tun]`，脚本会给出警告提示：
  在 macOS Docker Desktop 下常见于容器缺少 TUN 设备，不影响本脚本的控制面注册/PDU 信令验证结论。

## 4. 银河麒麟 V10(x86) 迁移清单

另见独立迁移文档：

- `docs/migration-macos-arm64-to-kylin-v10-x86.md`

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
- `ready_for_ueransim=true` 额外要求具备 UE 验证所需网元前置条件（`AUSF/UDM/UDR/PCF`）。
- 资源采样间隔统一为 `10-30s`（默认 `15s`），该间隔同时作用于计算资源与核心网业务负载刷新。
