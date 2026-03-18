# 动态卫星 SFC 项目数据库设计说明（MySQL 8.0）

## 1. 设计目标

本设计基于当前代码中的真实数据结构（后端 `types.h`、`SFCController`、`DynamicInferenceService`、`DynamicSimulationService`，前端 `useStore`、`SFCForm`、`useWebSocket`）抽取。

目标是让系统支持以下持久化能力：

- 保存拓扑与动态快照（可回放）。
- 保存 SFC 请求、候选方案、部署记录（可追溯）。
- 保存会话连续编排决策轨迹（可分析）。
- 保存故障/恢复事件与编排指标（可监控统计）。

## 2. 库表总览

推荐 16 张表，按模块分组。

- 拓扑模块
- `topology_scene`：一次拓扑场景（生成或导入）主记录。
- `topology_snapshot`：每个动态 tick 的快照头。
- `topology_node_state`：快照下每个节点状态。
- `topology_link_state`：快照下每条链路状态。

- SFC 请求与规划模块
- `sfc_request`：一次 `/sfc/plan` 或会话初始请求。
- `sfc_request_vnf`：请求中的 VNF 序列。
- `sfc_plan_result`：一次规划结果汇总。
- `sfc_plan_candidate`：规划候选（TopK）。
- `sfc_plan_candidate_vnf`：候选的 VNF 到节点映射。
- `sfc_plan_candidate_link`：候选的链路明细。

- 部署模块
- `sfc_deployment`：部署主记录（含状态流转）。
- `sfc_deployment_vnf`：部署时每个 VNF 的资源占用。

- 连续编排会话模块
- `sfc_session`：会话主记录。
- `sfc_session_decision`：每次会话决策轨迹（含 trigger、结果、时延）。

- 监控模块
- `runtime_event`：WebSocket 事件持久化（fault/recovery/session_update 等）。
- `orchestration_metrics_tick`：编排统计 tick（p50/p95/p99 等）。

## 3. 表设计（字段级）

### 3.1 `topology_scene`

- `id` BIGINT PK
- `scene_id` VARCHAR(64) UNIQUE，业务 ID（如 `topo_...`）
- `source_type` VARCHAR(16) NOT NULL，`generated|imported`
- `total_sats` INT NOT NULL
- `num_planes` INT NOT NULL
- `altitude_km` DECIMAL(8,3) NOT NULL
- `inclination_deg` DECIMAL(6,3) NOT NULL
- `sampling_interval_sec` DECIMAL(6,3) NOT NULL DEFAULT 5.0
- `initial_topology_version` INT NOT NULL DEFAULT 0
- `created_at` DATETIME(3) NOT NULL
- `updated_at` DATETIME(3) NOT NULL

索引：`idx_scene_created_at(created_at)`

### 3.2 `topology_snapshot`

- `id` BIGINT PK
- `scene_id` VARCHAR(64) NOT NULL
- `topology_version` INT NOT NULL
- `sim_time` DATETIME(3) NOT NULL
- `sampling_interval_sec` DECIMAL(6,3) NOT NULL
- `total_nodes` INT NOT NULL
- `active_nodes` INT NOT NULL
- `down_nodes` INT NOT NULL
- `total_links` INT NOT NULL
- `active_links` INT NOT NULL
- `down_links` INT NOT NULL
- `congested_links` INT NOT NULL
- `avg_latency_ms` DECIMAL(10,4) NOT NULL
- `avg_bandwidth_utilization` DECIMAL(8,6) NOT NULL
- `created_at` DATETIME(3) NOT NULL

唯一约束：`uk_scene_version(scene_id, topology_version)`

索引：`idx_scene_sim_time(scene_id, sim_time)`

### 3.3 `topology_node_state`

- `id` BIGINT PK
- `snapshot_id` BIGINT NOT NULL FK -> `topology_snapshot.id`
- `node_id` VARCHAR(64) NOT NULL
- `plane` INT NOT NULL
- `position_in_plane` INT NOT NULL
- `raan` DECIMAL(10,6) NOT NULL
- `true_anomaly` DECIMAL(10,6) NOT NULL
- `altitude_km` DECIMAL(8,3) NOT NULL
- `inclination_deg` DECIMAL(6,3) NOT NULL
- `x` DECIMAL(12,6) NOT NULL
- `y` DECIMAL(12,6) NOT NULL
- `z` DECIMAL(12,6) NOT NULL
- `lat` DECIMAL(9,6) NOT NULL
- `lon` DECIMAL(9,6) NOT NULL
- `cpu_total` DECIMAL(10,3) NOT NULL
- `cpu_available` DECIMAL(10,3) NOT NULL
- `mem_total` DECIMAL(10,3) NOT NULL
- `mem_available` DECIMAL(10,3) NOT NULL
- `disk_total` DECIMAL(10,3) NOT NULL
- `disk_available` DECIMAL(10,3) NOT NULL
- `core_network_load` DECIMAL(6,4) NOT NULL
- `node_reliability` DECIMAL(7,6) NOT NULL
- `status` VARCHAR(16) NOT NULL
- `fault_tag` VARCHAR(64) NOT NULL DEFAULT ''

唯一约束：`uk_snapshot_node(snapshot_id, node_id)`

索引：`idx_node_status(node_id, status)`

### 3.4 `topology_link_state`

- `id` BIGINT PK
- `snapshot_id` BIGINT NOT NULL FK -> `topology_snapshot.id`
- `source_node_id` VARCHAR(64) NOT NULL
- `target_node_id` VARCHAR(64) NOT NULL
- `link_type` VARCHAR(32) NOT NULL
- `status` VARCHAR(16) NOT NULL
- `latency_ms` DECIMAL(10,4) NOT NULL
- `reliability` DECIMAL(7,6) NOT NULL
- `bandwidth_gbps` DECIMAL(10,4) NOT NULL
- `bandwidth_available_gbps` DECIMAL(10,4) NOT NULL

唯一约束：`uk_snapshot_link(snapshot_id, source_node_id, target_node_id)`

索引：`idx_link_status(status)`

### 3.5 `sfc_request`

- `id` BIGINT PK
- `request_id` VARCHAR(64) UNIQUE NOT NULL
- `service_type` VARCHAR(64) NOT NULL
- `source_node` VARCHAR(64) NOT NULL
- `destination_node` VARCHAR(64) NOT NULL
- `priority` VARCHAR(16) NOT NULL
- `optimize` VARCHAR(32) NOT NULL
- `topk` INT NOT NULL
- `topology_version` INT NULL
- `sim_time` DATETIME(3) NULL
- `core_network_load` DECIMAL(6,4) NOT NULL
- `priority_weight` DECIMAL(8,4) NOT NULL
- `load_level` VARCHAR(16) NOT NULL
- `realtime_mode` TINYINT(1) NOT NULL DEFAULT 0
- `max_planning_attempts` INT NOT NULL DEFAULT 0
- `planning_time_budget_ms` DECIMAL(10,3) NOT NULL DEFAULT 0
- `max_latency_ms` DECIMAL(10,3) NOT NULL
- `min_bandwidth_gbps` DECIMAL(10,4) NOT NULL
- `min_reliability` DECIMAL(7,6) NOT NULL
- `score_weights_json` JSON NULL
- `raw_payload` JSON NULL
- `created_at` DATETIME(3) NOT NULL

索引：`idx_req_created(created_at)`，`idx_req_topology(topology_version, sim_time)`

### 3.6 `sfc_request_vnf`

- `id` BIGINT PK
- `request_id` VARCHAR(64) NOT NULL FK -> `sfc_request.request_id`
- `seq_no` INT NOT NULL
- `vnf_name` VARCHAR(64) NOT NULL
- `cpu` DECIMAL(10,3) NOT NULL
- `mem` DECIMAL(10,3) NOT NULL
- `disk` DECIMAL(10,3) NOT NULL
- `bw_in` DECIMAL(10,4) NOT NULL
- `bw_out` DECIMAL(10,4) NOT NULL

唯一约束：`uk_request_vnf_seq(request_id, seq_no)`

### 3.7 `sfc_plan_result`

- `id` BIGINT PK
- `request_id` VARCHAR(64) NOT NULL FK -> `sfc_request.request_id`
- `inference_time_ms` DECIMAL(10,3) NOT NULL
- `requested_topk` INT NOT NULL
- `returned_topk` INT NOT NULL
- `deployable_count` INT NOT NULL
- `fallback_only` TINYINT(1) NOT NULL
- `warning` VARCHAR(255) NULL
- `decision_process` JSON NULL
- `created_at` DATETIME(3) NOT NULL

索引：`idx_plan_request(request_id)`

### 3.8 `sfc_plan_candidate`

- `id` BIGINT PK
- `plan_result_id` BIGINT NOT NULL FK -> `sfc_plan_result.id`
- `candidate_index` INT NOT NULL
- `score` DECIMAL(12,6) NOT NULL
- `total_latency_ms` DECIMAL(10,4) NOT NULL
- `estimated_reliability` DECIMAL(7,6) NOT NULL
- `bottleneck_bandwidth_gbps` DECIMAL(10,4) NOT NULL
- `satisfies_constraints` TINYINT(1) NOT NULL
- `reason` VARCHAR(255) NULL
- `violation_details_json` JSON NULL
- `deployed_nodes_json` JSON NULL
- `score_breakdown_json` JSON NULL
- `created_at` DATETIME(3) NOT NULL

唯一约束：`uk_plan_candidate(plan_result_id, candidate_index)`

### 3.9 `sfc_plan_candidate_vnf`

- `id` BIGINT PK
- `candidate_id` BIGINT NOT NULL FK -> `sfc_plan_candidate.id`
- `seq_no` INT NOT NULL
- `vnf_name` VARCHAR(64) NOT NULL
- `node_id` VARCHAR(64) NOT NULL
- `cpu_used` DECIMAL(10,3) NOT NULL
- `mem_used` DECIMAL(10,3) NOT NULL
- `disk_used` DECIMAL(10,3) NOT NULL

唯一约束：`uk_candidate_vnf(candidate_id, seq_no)`

### 3.10 `sfc_plan_candidate_link`

- `id` BIGINT PK
- `candidate_id` BIGINT NOT NULL FK -> `sfc_plan_candidate.id`
- `seq_no` INT NOT NULL
- `src_node` VARCHAR(64) NOT NULL
- `dst_node` VARCHAR(64) NOT NULL
- `latency_ms` DECIMAL(10,4) NOT NULL
- `bandwidth_gbps` DECIMAL(10,4) NOT NULL
- `bandwidth_available_gbps` DECIMAL(10,4) NOT NULL
- `bandwidth_required_gbps` DECIMAL(10,4) NOT NULL
- `status` VARCHAR(16) NOT NULL
- `reliability` DECIMAL(7,6) NOT NULL

唯一约束：`uk_candidate_link(candidate_id, seq_no)`

### 3.11 `sfc_deployment`

- `id` BIGINT PK
- `deployment_id` VARCHAR(64) UNIQUE NOT NULL
- `backend_deployment_id` VARCHAR(64) NULL
- `request_id` VARCHAR(64) NOT NULL
- `session_id` VARCHAR(64) NULL
- `strategy_mode` VARCHAR(32) NOT NULL DEFAULT 'single_request'
- `candidate_index` INT NOT NULL
- `status` VARCHAR(24) NOT NULL
- `inference_latency_ms` DECIMAL(10,3) NULL
- `source_node` VARCHAR(64) NULL
- `destination_node` VARCHAR(64) NULL
- `total_latency_ms` DECIMAL(10,4) NOT NULL
- `bottleneck_bandwidth_gbps` DECIMAL(10,4) NULL
- `estimated_reliability` DECIMAL(7,6) NULL
- `satisfies_constraints` TINYINT(1) NULL
- `violation_details_json` JSON NULL
- `score_total` DECIMAL(12,6) NULL
- `score_breakdown_json` JSON NULL
- `score_weights_json` JSON NULL
- `score_constraints_json` JSON NULL
- `deployed_nodes_json` JSON NULL
- `path_nodes_json` JSON NULL
- `link_details_json` JSON NULL
- `path_recompute_count` INT NOT NULL DEFAULT 0
- `decision_trigger` VARCHAR(64) NULL
- `topology_version_bound` INT NULL
- `progress` INT NOT NULL DEFAULT 0
- `deployed_at` DATETIME(3) NOT NULL
- `updated_at` DATETIME(3) NOT NULL

索引：`idx_deploy_request(request_id)`，`idx_deploy_session(session_id)`，`idx_deploy_status(status, deployed_at)`

### 3.12 `sfc_deployment_vnf`

- `id` BIGINT PK
- `deployment_id` VARCHAR(64) NOT NULL FK -> `sfc_deployment.deployment_id`
- `seq_no` INT NOT NULL
- `vnf_id` VARCHAR(64) NOT NULL
- `vnf_type` VARCHAR(64) NOT NULL
- `node_id` VARCHAR(64) NOT NULL
- `cpu_used` DECIMAL(10,3) NOT NULL
- `mem_used` DECIMAL(10,3) NOT NULL
- `disk_used` DECIMAL(10,3) NOT NULL

唯一约束：`uk_deploy_vnf(deployment_id, seq_no)`

### 3.13 `sfc_session`

- `id` BIGINT PK
- `session_id` VARCHAR(64) UNIQUE NOT NULL
- `request_id` VARCHAR(64) NOT NULL
- `active` TINYINT(1) NOT NULL
- `auto_redeploy` TINYINT(1) NOT NULL
- `realtime_mode` TINYINT(1) NOT NULL
- `max_planning_attempts` INT NOT NULL
- `planning_time_budget_ms` DECIMAL(10,3) NOT NULL
- `source_node` VARCHAR(64) NOT NULL
- `destination_node` VARCHAR(64) NOT NULL
- `last_topology_version` INT NULL
- `last_sim_time` DATETIME(3) NULL
- `last_inference_time_ms` DECIMAL(10,3) NULL
- `decisions_total` INT NOT NULL DEFAULT 0
- `redeploy_total` INT NOT NULL DEFAULT 0
- `failures_total` INT NOT NULL DEFAULT 0
- `last_decision_trace` JSON NULL
- `created_at` DATETIME(3) NOT NULL
- `updated_at` DATETIME(3) NOT NULL

索引：`idx_session_active(active)`，`idx_session_request(request_id)`

### 3.14 `sfc_session_decision`

- `id` BIGINT PK
- `session_id` VARCHAR(64) NOT NULL FK -> `sfc_session.session_id`
- `request_id` VARCHAR(64) NOT NULL
- `trigger_type` VARCHAR(64) NOT NULL
- `status` VARCHAR(24) NOT NULL
- `topology_version` INT NOT NULL
- `sim_time` DATETIME(3) NOT NULL
- `source_node` VARCHAR(64) NULL
- `destination_node` VARCHAR(64) NULL
- `inference_time_ms` DECIMAL(10,3) NOT NULL
- `requested_topk` INT NOT NULL
- `returned_topk` INT NOT NULL
- `deployable_count` INT NOT NULL
- `fallback_only` TINYINT(1) NOT NULL
- `path_changed` TINYINT(1) NULL
- `reason` VARCHAR(255) NULL
- `decision_process` JSON NULL
- `candidates_json` JSON NULL
- `created_at` DATETIME(3) NOT NULL

索引：`idx_session_decision(session_id, created_at)`，`idx_decision_trigger(trigger_type)`

### 3.15 `runtime_event`

- `id` BIGINT PK
- `event_type` VARCHAR(32) NOT NULL
- `entity_type` VARCHAR(32) NULL
- `entity_id` VARCHAR(64) NULL
- `request_id` VARCHAR(64) NULL
- `session_id` VARCHAR(64) NULL
- `topology_version` INT NULL
- `sim_time` DATETIME(3) NULL
- `reason` VARCHAR(128) NULL
- `strategy` VARCHAR(64) NULL
- `result` VARCHAR(64) NULL
- `success` TINYINT(1) NULL
- `affected_services` INT NULL
- `payload` JSON NULL
- `created_at` DATETIME(3) NOT NULL

索引：`idx_event_type_time(event_type, created_at)`，`idx_event_session(session_id, created_at)`

### 3.16 `orchestration_metrics_tick`

- `id` BIGINT PK
- `topology_version` INT NOT NULL
- `sim_time` DATETIME(3) NOT NULL
- `active_sessions` INT NOT NULL
- `decisions_this_tick` INT NOT NULL
- `redeploys_this_tick` INT NOT NULL
- `failures_this_tick` INT NOT NULL
- `recovery_attempts_this_tick` INT NOT NULL
- `recovery_success_this_tick` INT NOT NULL
- `recovery_failures_this_tick` INT NOT NULL
- `total_decisions` BIGINT NOT NULL
- `total_redeploys` BIGINT NOT NULL
- `total_failures` BIGINT NOT NULL
- `total_recovery_attempts` BIGINT NOT NULL
- `total_recovery_success` BIGINT NOT NULL
- `total_recovery_failures` BIGINT NOT NULL
- `recovery_success_rate` DECIMAL(8,6) NOT NULL
- `latency_mean_ms` DECIMAL(10,4) NOT NULL
- `latency_p50_ms` DECIMAL(10,4) NOT NULL
- `latency_p95_ms` DECIMAL(10,4) NOT NULL
- `latency_p99_ms` DECIMAL(10,4) NOT NULL
- `created_at` DATETIME(3) NOT NULL

索引：`idx_metrics_sim_time(sim_time)`，`idx_metrics_topology(topology_version)`

## 4. 核心关系

- `sfc_request` 1:N `sfc_request_vnf`
- `sfc_request` 1:N `sfc_plan_result`
- `sfc_plan_result` 1:N `sfc_plan_candidate`
- `sfc_plan_candidate` 1:N `sfc_plan_candidate_vnf`
- `sfc_plan_candidate` 1:N `sfc_plan_candidate_link`
- `sfc_request` 1:N `sfc_deployment`
- `sfc_deployment` 1:N `sfc_deployment_vnf`
- `sfc_request` 1:N `sfc_session`
- `sfc_session` 1:N `sfc_session_decision`
- `topology_scene` 1:N `topology_snapshot`
- `topology_snapshot` 1:N `topology_node_state`
- `topology_snapshot` 1:N `topology_link_state`

## 5. MySQL 8.0 建表 SQL（核心版）

以下 SQL 给出最核心 8 张表，可先落地，再扩展其余分析表。

```sql
CREATE TABLE topology_scene (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  scene_id VARCHAR(64) NOT NULL,
  source_type VARCHAR(16) NOT NULL,
  total_sats INT NOT NULL,
  num_planes INT NOT NULL,
  altitude_km DECIMAL(8,3) NOT NULL,
  inclination_deg DECIMAL(6,3) NOT NULL,
  sampling_interval_sec DECIMAL(6,3) NOT NULL DEFAULT 5.0,
  initial_topology_version INT NOT NULL DEFAULT 0,
  created_at DATETIME(3) NOT NULL,
  updated_at DATETIME(3) NOT NULL,
  UNIQUE KEY uk_scene_id (scene_id),
  KEY idx_scene_created_at (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE topology_snapshot (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  scene_id VARCHAR(64) NOT NULL,
  topology_version INT NOT NULL,
  sim_time DATETIME(3) NOT NULL,
  sampling_interval_sec DECIMAL(6,3) NOT NULL,
  total_nodes INT NOT NULL,
  active_nodes INT NOT NULL,
  down_nodes INT NOT NULL,
  total_links INT NOT NULL,
  active_links INT NOT NULL,
  down_links INT NOT NULL,
  congested_links INT NOT NULL,
  avg_latency_ms DECIMAL(10,4) NOT NULL,
  avg_bandwidth_utilization DECIMAL(8,6) NOT NULL,
  created_at DATETIME(3) NOT NULL,
  UNIQUE KEY uk_scene_version (scene_id, topology_version),
  KEY idx_scene_sim_time (scene_id, sim_time),
  CONSTRAINT fk_snapshot_scene FOREIGN KEY (scene_id) REFERENCES topology_scene(scene_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE sfc_request (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  request_id VARCHAR(64) NOT NULL,
  service_type VARCHAR(64) NOT NULL,
  source_node VARCHAR(64) NOT NULL,
  destination_node VARCHAR(64) NOT NULL,
  priority VARCHAR(16) NOT NULL,
  optimize VARCHAR(32) NOT NULL,
  topk INT NOT NULL,
  topology_version INT NULL,
  sim_time DATETIME(3) NULL,
  core_network_load DECIMAL(6,4) NOT NULL,
  priority_weight DECIMAL(8,4) NOT NULL,
  load_level VARCHAR(16) NOT NULL,
  realtime_mode TINYINT(1) NOT NULL DEFAULT 0,
  max_planning_attempts INT NOT NULL DEFAULT 0,
  planning_time_budget_ms DECIMAL(10,3) NOT NULL DEFAULT 0,
  max_latency_ms DECIMAL(10,3) NOT NULL,
  min_bandwidth_gbps DECIMAL(10,4) NOT NULL,
  min_reliability DECIMAL(7,6) NOT NULL,
  score_weights_json JSON NULL,
  raw_payload JSON NULL,
  created_at DATETIME(3) NOT NULL,
  UNIQUE KEY uk_request_id (request_id),
  KEY idx_req_created (created_at),
  KEY idx_req_topology (topology_version, sim_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE sfc_request_vnf (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  request_id VARCHAR(64) NOT NULL,
  seq_no INT NOT NULL,
  vnf_name VARCHAR(64) NOT NULL,
  cpu DECIMAL(10,3) NOT NULL,
  mem DECIMAL(10,3) NOT NULL,
  disk DECIMAL(10,3) NOT NULL,
  bw_in DECIMAL(10,4) NOT NULL,
  bw_out DECIMAL(10,4) NOT NULL,
  UNIQUE KEY uk_request_vnf_seq (request_id, seq_no),
  CONSTRAINT fk_request_vnf_request FOREIGN KEY (request_id) REFERENCES sfc_request(request_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE sfc_plan_result (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  request_id VARCHAR(64) NOT NULL,
  inference_time_ms DECIMAL(10,3) NOT NULL,
  requested_topk INT NOT NULL,
  returned_topk INT NOT NULL,
  deployable_count INT NOT NULL,
  fallback_only TINYINT(1) NOT NULL,
  warning VARCHAR(255) NULL,
  decision_process JSON NULL,
  created_at DATETIME(3) NOT NULL,
  KEY idx_plan_request (request_id),
  CONSTRAINT fk_plan_request FOREIGN KEY (request_id) REFERENCES sfc_request(request_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE sfc_plan_candidate (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  plan_result_id BIGINT NOT NULL,
  candidate_index INT NOT NULL,
  score DECIMAL(12,6) NOT NULL,
  total_latency_ms DECIMAL(10,4) NOT NULL,
  estimated_reliability DECIMAL(7,6) NOT NULL,
  bottleneck_bandwidth_gbps DECIMAL(10,4) NOT NULL,
  satisfies_constraints TINYINT(1) NOT NULL,
  reason VARCHAR(255) NULL,
  violation_details_json JSON NULL,
  deployed_nodes_json JSON NULL,
  score_breakdown_json JSON NULL,
  created_at DATETIME(3) NOT NULL,
  UNIQUE KEY uk_plan_candidate (plan_result_id, candidate_index),
  CONSTRAINT fk_candidate_plan FOREIGN KEY (plan_result_id) REFERENCES sfc_plan_result(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE sfc_deployment (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  deployment_id VARCHAR(64) NOT NULL,
  backend_deployment_id VARCHAR(64) NULL,
  request_id VARCHAR(64) NOT NULL,
  session_id VARCHAR(64) NULL,
  strategy_mode VARCHAR(32) NOT NULL DEFAULT 'single_request',
  candidate_index INT NOT NULL,
  status VARCHAR(24) NOT NULL,
  inference_latency_ms DECIMAL(10,3) NULL,
  source_node VARCHAR(64) NULL,
  destination_node VARCHAR(64) NULL,
  total_latency_ms DECIMAL(10,4) NOT NULL,
  bottleneck_bandwidth_gbps DECIMAL(10,4) NULL,
  estimated_reliability DECIMAL(7,6) NULL,
  satisfies_constraints TINYINT(1) NULL,
  violation_details_json JSON NULL,
  score_total DECIMAL(12,6) NULL,
  score_breakdown_json JSON NULL,
  score_weights_json JSON NULL,
  score_constraints_json JSON NULL,
  deployed_nodes_json JSON NULL,
  path_nodes_json JSON NULL,
  link_details_json JSON NULL,
  path_recompute_count INT NOT NULL DEFAULT 0,
  decision_trigger VARCHAR(64) NULL,
  topology_version_bound INT NULL,
  progress INT NOT NULL DEFAULT 0,
  deployed_at DATETIME(3) NOT NULL,
  updated_at DATETIME(3) NOT NULL,
  UNIQUE KEY uk_deployment_id (deployment_id),
  KEY idx_deploy_request (request_id),
  KEY idx_deploy_session (session_id),
  KEY idx_deploy_status (status, deployed_at),
  CONSTRAINT fk_deploy_request FOREIGN KEY (request_id) REFERENCES sfc_request(request_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE sfc_session (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  session_id VARCHAR(64) NOT NULL,
  request_id VARCHAR(64) NOT NULL,
  active TINYINT(1) NOT NULL,
  auto_redeploy TINYINT(1) NOT NULL,
  realtime_mode TINYINT(1) NOT NULL,
  max_planning_attempts INT NOT NULL,
  planning_time_budget_ms DECIMAL(10,3) NOT NULL,
  source_node VARCHAR(64) NOT NULL,
  destination_node VARCHAR(64) NOT NULL,
  last_topology_version INT NULL,
  last_sim_time DATETIME(3) NULL,
  last_inference_time_ms DECIMAL(10,3) NULL,
  decisions_total INT NOT NULL DEFAULT 0,
  redeploy_total INT NOT NULL DEFAULT 0,
  failures_total INT NOT NULL DEFAULT 0,
  last_decision_trace JSON NULL,
  created_at DATETIME(3) NOT NULL,
  updated_at DATETIME(3) NOT NULL,
  UNIQUE KEY uk_session_id (session_id),
  KEY idx_session_active (active),
  KEY idx_session_request (request_id),
  CONSTRAINT fk_session_request FOREIGN KEY (request_id) REFERENCES sfc_request(request_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

## 6. 实施建议

- 第一阶段先落地 8 张核心表：`topology_scene`、`topology_snapshot`、`sfc_request`、`sfc_request_vnf`、`sfc_plan_result`、`sfc_plan_candidate`、`sfc_deployment`、`sfc_session`。
- 第二阶段补充明细表与监控表：`topology_node_state`、`topology_link_state`、`candidate_vnf/link`、`session_decision`、`runtime_event`、`orchestration_metrics_tick`。
- 高频写入表（`topology_snapshot`、`runtime_event`、`orchestration_metrics_tick`）建议按时间做分区或冷热分层。
- 前端 `useStore` 中仅展示用的衍生字段（如部分 UI 临时字段）不必强制归一化，可放 JSON。

