# 动态卫星 Open5GS 核心网智能编排系统

本项目面向动态卫星网络上的 Open5GS 核心网部署与持续编排，提供星座拓扑生成/导入、12 网元核心网策略生成、真实容器化网元拉起、故障注入、分级恢复、UERANSIM 功能验证和三维大屏可视化能力。

## 1. 主要能力

- 动态星座生成与第三方拓扑导入，导入结构与后端 `Topology` 模型保持一致。
- 在卫星星座上部署完整 Open5GS 核心网，覆盖 NRF、SCP、AMF、SMF、UPF、AUSF、UDM、UDR、PCF、BSF、NSSF、SEPP 等 12 个网元。
- 按 Open5GS 网元依赖关系计算网元间路径，而不是简单全图完全连通。
- 使用训练导出的 ONNX 模型生成核心网部署候选，并提供硬约束兜底策略。
- 启动真实 Open5GS 容器、Mongo 容器和 UERANSIM 验证容器。
- 支持节点/链路故障注入、路径重算、局部重调度、整体重调度和 SLA 持续监测。
- 大屏主页面、系统控制中心、监控中心、卫星/链路详情和消息面板联动展示。

## 2. 项目结构

```text
sfc_deploy/
├── backend/                   # C++ 后端（Drogon + ONNX Runtime）
├── frontend/                  # React + Vite + Three.js 前端
├── train/                     # 数据生成、训练、评估、导出
├── models/exported/           # 后端推理使用的 ONNX 模型
├── docker/                    # Open5GS 卫星节点容器构建文件
├── scripts/                   # 银河麒麟 V10 部署脚本
├── testdata/                  # 拓扑导入演示数据
├── KYLIN_V10_MIGRATION.md     # 银河麒麟 V10 完整迁移部署文档
└── dev.sh                     # 通用本地启动脚本
```

## 3. 推荐环境

### 银河麒麟 V10 桌面版（amd64）

已提供完整迁移文档：`KYLIN_V10_MIGRATION.md`

首次部署：

```bash
./scripts/kylin_v10_install_deps.sh
newgrp docker   # 若当前用户刚加入 docker 组，可执行；也可注销后重新登录
./scripts/kylin_v10_build.sh
./scripts/kylin_v10_start.sh
```

日常操作：

```bash
./scripts/kylin_v10_status.sh
./scripts/kylin_v10_stop.sh
./scripts/kylin_v10_start.sh
```

### 通用依赖

- Node.js 18+
- Python 3.10+
- CMake 3.16+
- C++17 编译器
- Docker
- Drogon
- spdlog
- nlohmann-json
- ONNX Runtime 1.16+

## 4. 本地快速启动

如果依赖已经安装完成，可以直接使用：

```bash
./dev.sh start
```

默认访问：

- 大屏主页面：`http://localhost:3001`
- 系统控制中心：`http://localhost:3002`
- 后端 API：`http://localhost:8080`

默认账号：

- 管理员：`admin / 123456`
- 普通用户：`user / 123456`

停止：

```bash
./dev.sh stop
```

## 5. 模型文件

后端默认读取：

```text
models/exported/gnn_encoder.onnx
models/exported/actor.onnx
models/exported/model_io_meta.json
```

如果模型缺失，可在训练侧导出后复制到 `models/exported/`。部署系统本身不要求重新训练。

## 6. 拓扑导入样例

当前提供一份可直接导入的 520 节点演示拓扑：

```text
testdata/third_party_constellation_import_520.json
```

该文件严格使用后端拓扑结构：

```json
{
  "metadata": {},
  "topology": {
    "nodes": [],
    "links": []
  }
}
```

其中包含 520 颗卫星和 1040 条 ISL，轨道内链路、轨道间链路均参与路径计算。

## 7. 训练与导出

安装训练依赖：

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r train/requirements.txt
```

一键训练流水线：

```bash
cd train
./start.sh
```

仅导出 ONNX：

```bash
cd train
python -m training.models.model_export --context-dim 48
```

## 8. 关键接口

后端 API 基础前缀：`/api/v1`

- 健康检查：`GET /health`
- 生成/导入拓扑：`POST /topology/generate`
- 动态仿真：`POST /topology/dynamic/start`、`GET /topology/dynamic/status`
- 核心网规划：`POST /sfc/plan`
- 核心网会话启动：`POST /sfc/session/start`
- 核心网会话停止：`POST /sfc/session/stop`
- 核心网会话列表：`GET /sfc/sessions`
- 重算/重调度：`POST /sfc/session/{id}/recompute`
- UERANSIM 验证：`/ueransim/*`
- WebSocket：`/ws/updates`

说明：部分 API 路径仍保留 `/sfc` 命名，这是历史兼容入口；当前业务语义已经切换为 Open5GS 核心网编排。

## 9. 运行数据

- `.run/logs`：后端和前端日志。
- `.run/pids`：进程 pid。
- `.run/mysql`：MySQL 容器数据卷。
- `.deps`：麒麟部署脚本下载/编译的依赖。

默认关闭 MySQL binlog，并在启动时清理过期事件，避免本地磁盘持续膨胀。

## 10. 常见问题

### 后端启动但前端无数据

- 检查后端健康状态：`curl http://127.0.0.1:8080/api/v1/health`
- 在控制中心生成或导入星座拓扑。
- 确认前端代理连接后端 `8080`。

### ONNX Runtime 加载失败

```bash
source .env.kylin
echo $ONNXRUNTIME_DIR
ls $ONNXRUNTIME_DIR/lib/libonnxruntime.so
```

### Docker 无权限

```bash
sudo systemctl enable --now docker
sudo usermod -aG docker $USER
newgrp docker
```

### Open5GS 镜像不可用

```bash
./scripts/kylin_v10_prepare_images.sh
```

远程镜像不可用时，脚本会尝试使用 `docker/open5gs-satellite.Dockerfile` 本地构建 `sfc-open5gs-satellite:local`。
