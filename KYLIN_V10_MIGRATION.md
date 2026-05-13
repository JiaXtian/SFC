# 银河麒麟操作系统 V10 桌面版迁移部署文档

本文档用于将当前“动态卫星 Open5GS 核心网智能编排系统”迁移到银河麒麟操作系统 V10 桌面版（类 Debian，amd64/x86_64）并完成完整部署。部署完成后，系统应具备星座拓扑生成/导入、Open5GS 12 网元核心网编排、容器启动、故障注入、分级恢复、UERANSIM 功能验证等能力。

## 1. 迁移目标

目标运行环境：

- 操作系统：银河麒麟操作系统 V10 桌面版，amd64 架构
- 部署方式：本机 C++ 后端 + 本机 React/Vite 前端 + Docker 容器运行 MySQL/Open5GS/Mongo/UERANSIM
- 默认端口：
  - 后端 API：`8080`
  - 大屏主页面：`3001`
  - 系统控制中心：`3002`
- 默认账号：
  - 管理员：`admin / 123456`
  - 普通用户：`user / 123456`

## 2. 系统组件

本系统在麒麟 V10 上由以下组件构成：

- `backend/`：C++17 后端，使用 Drogon、ONNX Runtime、spdlog、nlohmann-json。
- `frontend/`：React + Vite + Three.js 前端，包括大屏与控制中心两个入口。
- `models/exported/`：训练侧导出的 `gnn_encoder.onnx` 和 `actor.onnx`。
- `docker/open5gs-satellite.Dockerfile`：Open5GS 卫星节点容器的本地兜底构建文件。
- `.run/`：运行期目录，存放 pid、日志、MySQL 数据卷。
- `.deps/`：麒麟部署脚本下载/编译的第三方依赖目录。
- `.env.kylin`：麒麟部署脚本生成的本地环境变量文件。

## 3. 依赖清单

### 3.1 系统 apt 依赖

脚本会通过 `apt-get` 安装：

```bash
build-essential cmake make ninja-build pkg-config git curl wget ca-certificates
gnupg lsb-release unzip tar xz-utils jq lsof procps net-tools iproute2 iputils-ping
python3 python3-venv python3-pip python3-dev
libssl-dev libjsoncpp-dev uuid-dev zlib1g-dev libsqlite3-dev
nlohmann-json3-dev libspdlog-dev libbrotli-dev libyaml-cpp-dev
docker.io
```

### 3.2 项目内下载/编译依赖

首次执行 `scripts/kylin_v10_install_deps.sh` 时，会将以下依赖放到 `.deps/`：

- Node.js `20.11.1`，当系统 Node.js 低于 18 或不存在时下载。
- ONNX Runtime `1.16.0` Linux x64 预编译包。
- Drogon `v1.9.0` 源码，编译并安装到 `.deps/prefix`。
- 前端 npm 依赖，通过 `frontend/package-lock.json` 执行 `npm ci`。

可通过环境变量覆盖版本：

```bash
NODE_VERSION=20.11.1 \
ONNXRUNTIME_VERSION=1.16.0 \
DROGON_REF=v1.9.0 \
./scripts/kylin_v10_install_deps.sh
```

### 3.3 Docker 镜像

部署脚本会准备以下镜像：

- `mysql:8.0`：后端运行状态数据库。
- `mongo:6`：Open5GS 网元数据库。
- `docker.1ms.run/gradiant/open5gs:2.7.7`：默认 Open5GS 核心网网元镜像。
- `docker.io/free5gc/ueransim:latest`：UERANSIM 功能验证镜像。
- `sfc-open5gs-satellite:local`：当远程 Open5GS 镜像不可用时，从本仓库 Dockerfile 本地构建。

如果网络环境访问 Docker Hub 不稳定，可以先配置 Docker 镜像加速器，或设置：

```bash
export SFC_SATELLITE_IMAGE_AMD64=可访问的_open5gs_镜像
export UERANSIM_IMAGE=可访问的_ueransim_镜像
```

## 4. 首次部署步骤

### 4.1 获取代码

将项目放到麒麟机器，例如：

```bash
cd ~/projects
git clone <你的仓库地址> sfc_deploy
cd sfc_deploy
```

如果通过压缩包迁移，请保持目录结构完整，尤其是：

- `backend/`
- `frontend/`
- `models/exported/`
- `docker/`
- `testdata/`

### 4.2 安装依赖

```bash
cd ~/projects/sfc_deploy
./scripts/kylin_v10_install_deps.sh
```

该脚本会：

1. 安装系统 apt 包。
2. 启动并启用 Docker 服务。
3. 将当前用户加入 `docker` 组。
4. 下载 Node.js、ONNX Runtime。
5. 编译 Drogon 到 `.deps/prefix`。
6. 安装前端 npm 依赖。
7. 准备 MySQL、Mongo、Open5GS、UERANSIM 镜像。
8. 生成 `.env.kylin`。

如果是第一次加入 `docker` 组，执行完后需要注销并重新登录，或在当前终端执行：

```bash
newgrp docker
```

### 4.3 构建系统

```bash
cd ~/projects/sfc_deploy
./scripts/kylin_v10_build.sh
```

该脚本会：

- 使用 `.env.kylin` 中的 `ONNXRUNTIME_DIR`、`CMAKE_PREFIX_PATH`、`LD_LIBRARY_PATH`。
- 构建 `backend/build/sfc_server`。
- 执行前端生产构建校验。

### 4.4 启动系统

```bash
cd ~/projects/sfc_deploy
./scripts/kylin_v10_start.sh
```

启动后访问：

- 大屏主页面：`http://本机IP:3001`
- 系统控制中心：`http://本机IP:3002`
- 后端健康检查：`http://本机IP:8080/api/v1/health`

### 4.5 查看状态

```bash
./scripts/kylin_v10_status.sh
```

### 4.6 停止系统

```bash
./scripts/kylin_v10_stop.sh
```

该命令会停止后端和两个前端开发服务。MySQL、Mongo、Open5GS 部署容器不会被无条件删除，避免误删运行数据；核心网部署的回滚/清理由系统控制中心负责。

## 5. 运行目录说明

### 5.1 `.deps/`

存放麒麟部署所需第三方依赖：

- `.deps/onnxruntime-linux-x64-1.16.0`
- `.deps/prefix`，Drogon 安装目录
- `.deps/bin`，项目内 Node.js 软链接
- `.deps/downloads`，依赖压缩包缓存
- `.deps/src`，源码构建缓存

该目录可删除，删除后重新执行 `scripts/kylin_v10_install_deps.sh` 即可恢复。

### 5.2 `.run/`

存放运行期数据：

- `.run/pids`：后端与前端 pid。
- `.run/logs`：后端与前端日志。
- `.run/mysql`：MySQL 容器的数据卷。

注意：`.run/mysql` 是系统数据库数据，不要在有部署记录需要保留时直接删除。若需要重置系统数据库，可先停止系统和 MySQL 容器，再备份或删除 `.run/mysql`。

## 6. 模型文件要求

后端默认读取：

```text
models/exported/gnn_encoder.onnx
models/exported/actor.onnx
models/exported/model_io_meta.json
```

迁移前请确认这些文件存在：

```bash
ls -lh models/exported/
```

如果模型缺失，可以在原机器训练导出后复制，也可以在麒麟机器安装训练依赖后执行训练导出流程。部署运行不要求重新训练。

## 7. 拓扑导入演示

项目提供麒麟可直接导入的 520 节点样例：

```text
testdata/third_party_constellation_import_520.json
```

该文件严格匹配后端拓扑结构：

```json
{
  "metadata": {},
  "topology": {
    "nodes": [],
    "links": []
  }
}
```

在系统控制中心点击“导入第三方构型”后选择该文件，应能导入 520 颗卫星、1040 条 ISL，其中包含轨道内链路和轨道间链路。

## 8. Open5GS 核心网编排部署

系统核心网部署流程：

1. 前端生成或导入卫星拓扑。
2. 系统控制中心创建 Open5GS 核心网部署请求。
3. 后端使用 ONNX 模型和约束兜底搜索生成候选策略。
4. 用户选择候选并部署。
5. 后端为承载网元的卫星节点启动 Open5GS 容器。
6. Mongo 容器提供 Open5GS 数据库。
7. 系统持续监测拓扑变化、链路故障、节点故障和 SLA。
8. 触发路径重算、局部重调度或整体重调度。
9. 可在 UERANSIM 页面执行注册/PDU Session/UE 连通验证。

部署所需 Docker 网络：

- `sfc-net`：MySQL 管理网络。
- `sfc-open5gs-net`：Open5GS、Mongo、UERANSIM 网络。

脚本会自动创建这两个网络。

## 9. 离线/内网部署建议

如果麒麟机器无法访问公网，可以在有网机器提前下载：

- Node.js tarball：`node-v20.11.1-linux-x64.tar.xz`
- ONNX Runtime：`onnxruntime-linux-x64-1.16.0.tgz`
- Drogon 源码包或 Git 仓库镜像
- Docker 镜像：
  - `mysql:8.0`
  - `mongo:6`
  - Open5GS 镜像
  - UERANSIM 镜像

导出镜像：

```bash
docker save mysql:8.0 mongo:6 docker.1ms.run/gradiant/open5gs:2.7.7 docker.io/free5gc/ueransim:latest \
  -o sfc-kylin-images.tar
```

导入镜像：

```bash
docker load -i sfc-kylin-images.tar
```

将下载文件放入 `.deps/downloads/` 后再次执行安装脚本，脚本会优先复用已有文件。

## 10. 常用脚本

| 脚本 | 用途 |
| --- | --- |
| `scripts/kylin_v10_install_deps.sh` | 首次安装 apt 依赖、Node、ONNX Runtime、Drogon、npm 依赖和镜像 |
| `scripts/kylin_v10_prepare_images.sh` | 单独准备 Docker 网络和镜像 |
| `scripts/kylin_v10_build.sh` | 构建后端并校验前端构建 |
| `scripts/kylin_v10_start.sh` | 启动 MySQL、后端、大屏、控制中心 |
| `scripts/kylin_v10_stop.sh` | 停止后端和前端 |
| `scripts/kylin_v10_status.sh` | 查看进程、容器和后端健康状态 |
| `dev.sh` | 通用开发启动脚本，麒麟脚本会在设置环境后调用它 |

## 11. 常见问题

### 11.1 Docker 无权限

现象：

```text
permission denied while trying to connect to the Docker daemon socket
```

处理：

```bash
sudo systemctl enable --now docker
sudo usermod -aG docker $USER
newgrp docker
```

如果仍无效，请注销并重新登录。

### 11.2 ONNX Runtime 找不到

确认：

```bash
source .env.kylin
echo $ONNXRUNTIME_DIR
ls $ONNXRUNTIME_DIR/lib/libonnxruntime.so
```

然后重新构建：

```bash
./scripts/kylin_v10_build.sh
```

### 11.3 Drogon 找不到

确认：

```bash
source .env.kylin
find .deps/prefix -name 'DrogonConfig.cmake'
```

若不存在，重新执行：

```bash
./scripts/kylin_v10_install_deps.sh
```

### 11.4 Open5GS 镜像拉取失败

可直接本地构建：

```bash
UBUNTU_MIRROR=http://archive.ubuntu.com/ubuntu \
OPEN5GS_REF=v2.7.7 \
./scripts/kylin_v10_prepare_images.sh
```

如果网络在国内环境较慢，可以将 `UBUNTU_MIRROR` 替换为可访问的 Ubuntu jammy 镜像源。

### 11.5 前端无法连接后端

检查：

```bash
curl http://127.0.0.1:8080/api/v1/health
./scripts/kylin_v10_status.sh
```

确认防火墙未拦截 `3001`、`3002`、`8080`。桌面本机访问通常不需要额外开放；局域网访问可按麒麟防火墙工具开放端口。

### 11.6 MySQL 数据过大

本项目默认关闭 MySQL binlog，并在 `dev.sh` 启动时清理过期事件。数据目录为：

```text
.run/mysql
```

若要完全重置数据库：

```bash
./scripts/kylin_v10_stop.sh
docker rm -f sfc-mysql
mv .run/mysql .run/mysql.backup.$(date +%Y%m%d%H%M%S)
./scripts/kylin_v10_start.sh
```

## 12. 验收步骤

部署完成后建议按以下顺序验收：

1. 执行 `./scripts/kylin_v10_status.sh`，后端健康检查返回正常。
2. 打开 `http://本机IP:3001`，大屏显示星球和卫星节点。
3. 打开 `http://本机IP:3002`，使用 `admin / 123456` 登录。
4. 在系统控制中心导入 `testdata/third_party_constellation_import_520.json`。
5. 生成 Open5GS 核心网部署策略，应返回可部署候选。
6. 选择候选部署，观察容器启动进度。
7. 在 Docker 中看到 `sfc-open5gs-mongo` 和若干 `sfc-sat-*` 容器。
8. 注入链路故障，观察路径重算。
9. 注入单个承载网元卫星节点故障，观察局部重调度。
10. 注入同一核心网两个及以上承载卫星故障，观察整体重调度且 CORE 标识保持不变。
11. 在 UERANSIM 验证页面执行功能验证。

## 13. 生产化建议

- 修改 `backend/config.json` 中的 JWT secret。
- 将 `admin / 123456` 默认密码改为强密码。
- 若长期运行，建议将 `.run/mysql` 放在独立磁盘或可备份目录。
- 若部署给局域网用户访问，建议使用 Nginx/HTTPS 反向代理。
- 保留 `models/exported/` 的模型版本记录，避免训练产物和后端推理配置不匹配。
