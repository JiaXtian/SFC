# SFC 系统迁移简表：macOS(arm64) -> 银河麒麟 V10(x86)

## 1. 目标

将当前在 macOS(arm64) 运行的“卫星容器化 Open5GS 编排系统”迁移到银河麒麟 V10(x86)，并保持：
- 前端“确认部署即完成”交互不变
- 后端异步编排不变
- Open5GS 核心网容器联通与服务可用性不变
- `verify_ueransim_smoke.sh` 手动验证流程不变

## 2. 迁移前准备

- 安装 Docker Engine（含 buildx / compose plugin）
- 安装基础工具：`bash` `curl` `jq`
- 确认镜像仓库可访问（Open5GS 卫星镜像、`docker.io/free5gc/ueransim:latest`）

建议内核参数（一次性）：

```bash
sudo modprobe br_netfilter
sudo sysctl -w net.ipv4.ip_forward=1
sudo sysctl -w net.bridge.bridge-nf-call-iptables=1
sudo sysctl -w net.bridge.bridge-nf-call-ip6tables=1
```

## 3. 镜像与架构策略

推荐使用多架构统一镜像标签（manifest list）：

```bash
IMAGE_REPO=ghcr.io/<your-org>/sfc-open5gs-satellite \
IMAGE_TAG=v0.1.0 \
./scripts/build_open5gs_multiarch.sh
```

在麒麟 x86 上配置：

```bash
export SFC_SATELLITE_IMAGE_AMD64=ghcr.io/<your-org>/sfc-open5gs-satellite:v0.1.0
# 或统一：
# export SFC_SATELLITE_IMAGE=ghcr.io/<your-org>/sfc-open5gs-satellite:v0.1.0
```

## 4. 应用迁移步骤

1. 迁移代码与模型目录到麒麟主机  
2. 在麒麟上构建后端并启动后端/前端  
3. 确认 Docker 网络 `sfc-open5gs-net` 可创建  
4. 发起一次 SFC 部署并观察 `/api/v1/deployments`：
   - `orchestration_phase` 最终应为 `running`
   - `service_ready=true`
   - `ready_for_ueransim=true`
5. 执行 UERANSIM 验证：

```bash
API_BASE=http://127.0.0.1:18080/api/v1 \
# 可直接传 JWT，或依赖脚本默认账号自动登录（admin/123456）
# API_TOKEN=<jwt_token> \
STRICT_PDU_SESSION=1 \
UERANSIM_IMAGE=docker.io/free5gc/ueransim:latest \
./scripts/verify_ueransim_smoke.sh
```

说明：
- 脚本默认使用 `SST=1`、`SD=000001` 与 Open5GS 对接
- 未设置 `API_BASE` 时，脚本会自动探测 `18080/8080` 两个本地端口
- 默认仅验证 `ready_for_ueransim=true`；如需强制测试 `service_ready=true` 的部署，可设置 `ALLOW_SERVICE_READY_FALLBACK=1`
- 可选开启 `STRICT_PDU_SESSION=1`，要求 UE 注册后必须在 `PDU_WAIT_SEC` 内完成 PDU 会话日志验证
- 如你自定义切片，可显式设置 `UERANSIM_SST/UERANSIM_SD`

## 5. 验收最小清单

- 模板使用 `SA-Full-12`，并确认 12 种核心网网元（含 `BSF/SEPP`）均可拉起
- 部署涉及卫星容器成功启动，未涉及卫星容器保持停止
- 容器命名满足：`sfc-sat-<deployment_id>-<satellite_node_id>`
- `containers_running == containers_total`
- `core_nfs_running == core_nfs_total`
- `service_ready=true`
- UERANSIM 脚本输出 `[OK] UERANSIM smoke verification passed`
- 资源采样间隔配置在 `10-30s`（默认 `15s`），并能驱动 CPU/MEM/DISK 与核心网业务负载同步刷新

## 6. 常见问题

- 镜像拉取慢或失败：优先检查企业网络策略、代理和 DNS
- x86 节点上误用 arm64 镜像：确认设置了 `SFC_SATELLITE_IMAGE_AMD64`
- UERANSIM 无法接入：先确认部署状态 `ready_for_ueransim=true` 再执行脚本
- UE 日志出现 `/dev/net/tun` 错误：在 macOS Docker Desktop 上常见，不影响控制面注册与 PDU 信令冒烟结论；在麒麟生产环境建议开启 TUN 设备能力
