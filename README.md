# 动态卫星 SFC 智能编排系统

一个面向动态卫星网络的 SFC（Service Function Chaining）编排与可视化系统，提供从数据生成、模型训练、ONNX 导出、后端推理到前端实时展示的完整闭环。

## 1. 主要能力
- 动态星座生成（Walker-Delta 参数化）。
- 卫星位置与 ISL 链路动态更新。
- SFC 候选策略生成、手动选择初始部署、持续编排维护。
- 故障注入与恢复（节点/链路故障、恢复事件流）。
- 监控中心展示拓扑健康、编排时延、恢复质量与稳定性。
- 训练产物导出 ONNX，供 C++ 后端在线推理。

## 2. 项目结构
```text
sfc_deploy/
├── backend/                   # C++ 后端（Drogon + ONNX Runtime）
├── frontend/                  # React + Vite + Three.js 前端
├── train/                     # 数据生成、训练、评估、导出
├── models/
│   ├── checkpoints/           # 训练权重（.pth）
│   └── exported/              # ONNX 模型
├── logs/                      # 训练与验证日志
├── 算法训练与推理说明.md
└── 项目总体设计文档.md
```

## 3. 环境要求

### 3.1 通用
- Node.js 18+
- Python 3.10+
- CMake 3.16+
- C++17 编译器（clang++/g++）

### 3.2 后端依赖
- Drogon
- spdlog
- nlohmann-json
- ONNX Runtime 1.16+

后端配置文件：`/Users/t1an/Desktop/project/SFC/sfc_deploy/backend/config.json`
默认模型路径：
- `../models/exported/gnn_encoder.onnx`
- `../models/exported/actor.onnx`

## 4. 快速启动（仅运行推理与可视化）

### 4.1 启动后端
```bash
cd /Users/t1an/Desktop/project/SFC/sfc_deploy/backend
./build.sh
cd build
./sfc_server
```

Ubuntu 可使用：
```bash
cd /Users/t1an/Desktop/project/SFC/sfc_deploy/backend
./build_ubuntu.sh --release
cd build
LD_LIBRARY_PATH=$ONNXRUNTIME_DIR/lib:$LD_LIBRARY_PATH ./sfc_server
```

### 4.2 启动前端
```bash
cd /Users/t1an/Desktop/project/SFC/sfc_deploy/frontend
npm install
npm run dev
```

默认访问（以 Vite 输出为准）：
- 前端：`http://localhost:5173`
- 后端：`http://localhost:8080`

## 5. 训练与导出完整流程

### 5.1 安装训练依赖
```bash
cd /Users/t1an/Desktop/project/SFC/sfc_deploy
python3 -m venv venv
source venv/bin/activate
pip install -r train/requirements.txt
```

### 5.2 一键训练流水线（数据生成→训练→评估→导出→推理测试）
```bash
cd /Users/t1an/Desktop/project/SFC/sfc_deploy/train
./start.sh
```

常见跳过参数示例：
```bash
# 仅使用已有模型做推理链路验证
./start.sh --skip-data --skip-train --skip-val-eval --skip-export
```

### 5.3 单独导出 ONNX
```bash
cd /Users/t1an/Desktop/project/SFC/sfc_deploy/train
python -m ground_training.models.model_export --context-dim 48
```

导出结果：
- `/Users/t1an/Desktop/project/SFC/sfc_deploy/models/exported/gnn_encoder.onnx`
- `/Users/t1an/Desktop/project/SFC/sfc_deploy/models/exported/actor.onnx`

## 6. 关键接口
后端 API 基础前缀：`/api/v1`

- 健康检查：`GET /health`
- 生成拓扑：`POST /topology/generate`
- 动态仿真：
  - `POST /topology/dynamic/start`
  - `POST /topology/dynamic/stop`
  - `GET /topology/dynamic/status`
- SFC 规划：`POST /sfc/plan`
- SFC 部署：`POST /sfc/deploy`
- 回滚：`POST /sfc/rollback`
- 会话连续编排：
  - `POST /sfc/session/start`
  - `POST /sfc/session/stop`
  - `GET /sfc/sessions`
  - `POST /sfc/session/{id}/recompute`
- WebSocket：`/ws/updates`

## 7. 推荐运行顺序
1. 启动后端。
2. 启动前端。
3. 前端生成/导入星座。
4. 创建 SFC 请求并生成候选方案。
5. 手动选择初始方案部署。
6. 观察动态拓扑、路径重算与必要重调度。
7. 在系统监控中心查看稳定性与恢复指标。

## 8. 常见问题

### 8.1 后端启动但前端无数据
- 检查是否已生成拓扑（后端默认不自动加载拓扑）。
- 检查前端代理是否正确连接 `:8080`。

### 8.2 ONNX 模型加载失败
- 确认 `models/exported` 下模型存在。
- 确认 `backend/config.json` 的模型路径与当前目录结构一致。

### 8.3 推理结果看起来合理但未训练
- 后端包含启发式与硬约束兜底，未训练也可能有可行结果。
- 重新训练可显著提升动态场景下的排序质量、稳定性与时延表现。

## 9. 相关文档
- 详细设计：`/Users/t1an/Desktop/project/SFC/sfc_deploy/项目总体设计文档.md`
- 训练与推理说明：`/Users/t1an/Desktop/project/SFC/sfc_deploy/算法训练与推理说明.md`
- 后端说明：`/Users/t1an/Desktop/project/SFC/sfc_deploy/backend/README.md`
