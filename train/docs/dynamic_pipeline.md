# 动态数据与训练闭环（v2）

## 1. 生成 4800 星动态场景
```bash
cd train/ground_training/data_generation
python dynamic_scene_generator.py \
  --total-sats 4800 \
  --num-planes 60 \
  --step-sec 5 \
  --duration-sec 300 \
  --output ../../data/dynamic/scenes/scene_4800_v1.json
```

## 2. 生成动态训练数据清单
```bash
cd train/ground_training/data_generation
python generate_dynamic_dataset.py \
  --scene-file ../../data/dynamic/scenes/scene_4800_v1.json \
  --dataset-file ../../data/dynamic/dataset_v2_index.json \
  --train-ratio 0.8
```

## 3. 字段口径
- 与后端 `TopologySnapshot` 对齐：
`sim_time/topology_version/sampling_interval_sec/topology.nodes/topology.links/metrics/events`

## 4. 说明
- 动态场景通过 `SGP4` 进行轨道推进。
- 同步输出 `skyfield_tt`，便于后续与 Skyfield 时间轴进行严格对齐分析。

## 5. 阶段2动态训练
```bash
cd train
python3 ground_training/train_dynamic.py \
  --dynamic_topology_dir data/train/dynamic/topologies \
  --dynamic_request_dir data/train/dynamic/requests \
  --epochs 40 \
  --history_window 4 \
  --context_dim 80
```

输出物：
- `logs/dynamic_training_metrics.json`
- `logs/dynamic_validation_report.json`
- `models/checkpoints/model_dynamic_best.pth`
- `models/checkpoints/gnn_dynamic_best.pth`
- `models/exported_dynamic/actor.onnx`
- `models/exported_dynamic/gnn_encoder.onnx`

## 6. 动态验证
```bash
cd train
python3 ground_training/evaluate_dynamic.py \
  --model_checkpoint models/checkpoints/model_dynamic_best.pth \
  --gnn_checkpoint models/checkpoints/gnn_dynamic_best.pth \
  --context_dim 80 \
  --history_window 4
```
