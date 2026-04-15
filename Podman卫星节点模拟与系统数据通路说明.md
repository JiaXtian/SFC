# Podman卫星节点模拟与系统数据通路说明

## 1. 当前 Podman 卫星节点模拟是怎么工作的

系统当前采用“**逻辑资源模拟 + 轻量容器承载**”模式：

1. 前端在“卫星构型生成与导入”提交拓扑到 `/api/v1/topology/generate`。
2. 后端根据模板/导入拓扑生成卫星节点与链路。
3. `SatelliteRuntimeService` 为每个卫星节点创建对应 Podman 容器（容器名 `sfc-sat-<sat_id>`）。
4. 节点 CPU/MEM/DISK 占用不是读取宿主机真实占用，而是按节点 ID + 时间戳做可重复的随机波动模拟（便于大规模仿真）。
5. 节点故障注入、部署策略生效（含回滚）通过节点字段落地：
   - `fault_injected`, `fault_tag`
   - `deployment_state`, `deployment_detail`
   - `core_nf_policy`, `core_nf_policy_applied`

这样既能保留“每节点一个 Podman 实体”的工程语义，又避免真实资源测量带来的高开销。

## 2. 为什么现在能比之前更快

本次改动后：

1. 增加了并行创建/启动/停止（可配置并发度）。
2. 停止/删除支持批量命令分块执行（不是逐个串行）。
3. 默认启用轻量模式 `lightweight_create_only=true`：
   - 生成阶段优先创建容器元数据，不强制全部立即运行。
   - 资源状态由仿真模块持续更新。

关键配置位于 `/Users/t1an/Desktop/project/SFC/sfc_deploy/backend/config.json` 的 `podman` 段：

- `cpu_limit_per_sat`
- `mem_limit_mb_per_sat`
- `tmpfs_limit_mb_per_sat`
- `pids_limit_per_sat`
- `lightweight_create_only`
- `provision_parallelism`
- `start_parallelism`
- `stop_parallelism`

## 3. 未来是否可在卫星节点中部署 open5GS 核心网网元

可行，建议分两阶段：

1. **当前阶段（已实现）**：策略级部署仿真
   - 仅在节点字段标记“某网元已部署/已回滚”，不真正在容器内起 open5GS。
2. **后续阶段（可扩展）**：真实网元容器化下沉
   - 在卫星容器中改为 sidecar 或同网络命名空间方式启动网元容器。
   - 引入网元生命周期编排（健康检查、依赖、重启策略、版本回滚）。

结论：当前架构已经预留了策略状态与节点映射，可平滑升级到真实 open5GS 部署。

## 4. 当前系统数据通路

1. 前端控制页发起操作（生成星座、注入故障、策略部署、启停节点）。
2. 后端 Controller 接口接收请求并调用：
   - `TopologyManager`（拓扑）
   - `SatelliteRuntimeService`（Podman 与节点运行态）
   - `DynamicSimulationService`（动态演化）
   - `PersistenceService`（持久化）
3. 运行态与拓扑快照写入 MySQL。
4. 前端通过轮询接口读取：
   - `/api/v1/satellites/runtime/status`（含部署进度）
   - `/api/v1/satellites`（节点详情）
5. 页面实时刷新进度、节点状态、故障状态和策略状态。

## 5. MySQL 在系统中的作用

MySQL 是唯一持久化数据库（Neo4j 相关逻辑已移除），主要负责：

1. 保存星座运行批次（`constellation_runs`）。
2. 保存每颗卫星最新状态（`satellite_state`）。
3. 保存链路状态（`link_state`）。
4. 保存关键事件日志（`event_log`）。

这保证了系统重启后可恢复最新状态，而不是回到纯内存初始状态。

## 6. 启动与严格连库说明

### 6.1 启动 MySQL（Docker）

系统脚本会管理容器 `sfc-mysql`：

```bash
cd /Users/t1an/Desktop/project/SFC/sfc_deploy
./start_system.sh
```

### 6.2 严格连库模式

后端已启用：

- `persistence.enabled=true`
- `persistence.strict_startup=true`

含义：MySQL 不可达/建表失败时，后端直接启动失败，不会“带病运行”。

### 6.3 连接检查

```bash
docker ps | grep sfc-mysql
curl http://127.0.0.1:8080/api/v1/persistence/status
```

若后端正常，`/persistence/status` 会返回 `mode=mysql_only` 且 `initialized=true`。

---

如果后续你要把 open5GS 真正部署进卫星节点，我建议下一步先做：

1. 选 3~5 个节点做最小可运行 PoC（AMF/SMF/UPF）。
2. 给每个网元加健康检查与回滚策略。
3. 再把部署器扩展成可批量编排版本。
