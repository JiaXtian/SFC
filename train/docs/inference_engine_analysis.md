# 推理引擎核心逻辑与异常现象分析

本文基于当前仓库中的星上推理实现、训练环境实现，以及现有推理结果文件进行逐段分析，回答以下问题：

1. actor 网络到底在做什么
2. 候选节点集合是如何生成的
3. 为什么有时明明感觉有可部署方案，却显示“无候选节点”
4. 为什么当前节点资源足够，却没有被选中甚至没有被尝试
5. 为什么推理结果里会出现很多超长路径
6. 为什么“概率最高”的策略不一定是最优策略

---

## 1. 结论先行

当前推理引擎不是“actor 直接在全图选一个最优节点”，而是一个三阶段流水线：

1. 启发式裁剪先生成候选集合。
2. actor 只在这个候选集合上输出概率分布并选一个首选动作。
3. 编排器从 actor 首选开始，按固定回退顺序最多探测 10 个候选，逐个做硬约束校验。

因此：

- `无候选节点` 发生在 actor 之前，根因来自启发式裁剪，不是 actor 输出错误。
- `actor 最高概率 != 全局最优` 是正常现象，因为 actor 只在被裁剪后的子集上打分，而且它优化的是训练学到的期望回报，不是当前实例上的精确约束最优化。
- `当前节点可部署但未被使用` 也是正常现象，因为“当前节点”不会被强制保底加入，也不会被强制优先尝试。

---

## 2. 推理引擎完整执行链路

### 2.1 图编码阶段

推理启动后，主程序先读取拓扑和请求，然后为每个拓扑计算一次 GNN 节点嵌入，之后复用于该拓扑上的全部请求。[main.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/main.cpp#L293) [main.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/main.cpp#L307)

节点特征包含：

- CPU 可用比
- MEM 可用比
- DISK 可用比
- core load
- 节点可靠性
- 出边存活率
- 出边带宽余量比
- 出边平均时延归一化

实现见：[network_graph.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/network_graph.cpp#L107)

### 2.2 每个 VNF 的部署流程

对每个 VNF，推理引擎执行：

1. 计算剩余时延预算 `remaining_delay = max_latency - accumulated_delay`。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L184)
2. 调用启发式裁剪器生成候选节点集合。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L201)
3. 将候选节点索引、VNF 特征、上下文特征送入 actor，得到候选子集上的概率分布并取最大概率动作。[drl_inference.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/drl_inference.cpp#L74)
4. 从 actor 首选开始构造回退顺序，但最多只探测 10 个候选。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L254)
5. 每次探测时，重新检查：
   - 节点资源
   - 到候选节点的最短可行带宽路径
   - 部署后累计时延
   - 前瞻可靠性
6. 某个候选通过后，扣减节点资源和链路带宽，继续下一个 VNF。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L331)
7. 全部 VNF 部署结束后，再单独计算最后一个 VNF 节点到目的节点的收尾路径，并检查最终时延与可靠性。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L377)

---

## 3. actor 网络的真实作用

actor 网络不是生成候选集合，它只负责在“已经给定的候选集合”上排序。

训练侧 actor 的前向逻辑是：

- 取出 `candidate_indices` 对应的候选节点嵌入
- 把每个候选节点嵌入与 `vnf_features`、`context_features` 拼接
- 输出每个候选的 logit
- 对 logits 做 softmax，得到候选集合内部的概率分布

实现见：[drl_agent.py](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/ground_training/models/drl_agent.py#L26)

推理侧 ONNX actor 的调用逻辑是：

- 输入 `node_embeddings`
- 输入 `candidate_indices`
- 输入 `vnf_features`
- 输入 `context_features`
- 输出 `probs` 和 `logits`
- 直接取 `probs` 最大的那个候选作为 `best_action`

实现见：[drl_inference.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/drl_inference.cpp#L118)

所以 actor 的职责可以准确表述为：

> 在启发式裁剪后的候选子集上，学习一个“哪个候选更值得优先尝试”的排序器。

它不负责：

- 枚举全图节点
- 证明全局最优
- 保证硬约束必然满足
- 给当前节点保底

---

## 4. 候选节点集合是如何生成的

推理侧候选集合完全由 `HeuristicPruner::get_candidate_nodes` 生成。[heuristic_pruner.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/heuristic_pruner.cpp#L68)

### 4.1 第一步：计算两类最短时延距离

裁剪器先算两张距离表：

- `dist_from_prev`：从当前前置节点 `prev_node` 到全图所有节点的最短时延
- `dist_to_dest`：从目的节点 `dest_node` 反向回推到全图所有节点的最短时延

实现见：[heuristic_pruner.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/heuristic_pruner.cpp#L74)

### 4.2 第二步：逐节点过滤

每个节点要同时满足以下条件才会进入候选集合：

1. 节点资源足够：CPU/MEM/DISK 都满足当前 VNF 需求。[heuristic_pruner.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/heuristic_pruner.cpp#L79)
2. `prev_node -> node` 可达，且 `node -> dest_node` 可达。[heuristic_pruner.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/heuristic_pruner.cpp#L84)
3. `d1 + d2 <= remaining_delay`，即“从当前点走到候选节点，再从候选节点走到最终目的节点”的最短时延总和不能超过当前剩余时延预算。[heuristic_pruner.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/heuristic_pruner.cpp#L90)

注意，这里只看了：

- 节点资源
- 链路连通性
- 时延可达性

这里**没有**检查：

- 链路带宽是否足够
- 当前路径的真实可靠性是否满足
- 候选节点是否一定是全局最优

### 4.3 第三步：打分并截断

通过过滤的节点还会被打一个启发式分数：

- `resource_score = 0.5 * cpu_util + 0.3 * mem_util + 0.2 * disk_util`
- `latency_score = (dist_from_prev + dist_to_dest) / 1000`
- `final_score = w_res * resource_score + w_lat * latency_score`

实现见：[heuristic_pruner.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/heuristic_pruner.cpp#L57)

然后按分数升序排序，只保留前 `top_m` 个。[heuristic_pruner.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/heuristic_pruner.cpp#L98)

所以当前候选集合的准确口径是：

> 资源足够、从当前点可达、到目的点可达、最短时延总和不超剩余预算，并且在启发式分数上进入前 `top_m` 的节点。

---

## 5. “当前节点资源足够，为什么不直接作为方案”

### 5.1 当前实现其实支持“原地部署”

如果选中的部署节点就是 `prev_node`，则：

- `find_shortest_path(prev, prev)` 会返回单节点路径 `[prev]`
- 路径时延是 0
- 路径可靠性是 1

实现见：[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L43) [sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L124)

训练环境同样显式支持 same-node deploy。[sfc_env.py](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/ground_training/environment/sfc_env.py#L186)

所以问题不是“代码不支持当前节点部署”，而是：

### 5.2 当前节点不会被保底加入

当前节点 `prev_node` 只是普通候选之一，只有在下面全部成立时才会进入候选集合：

1. 当前节点资源足够
2. 当前节点到目的节点可达
3. `0 + dist(prev, dest) <= remaining_delay`
4. 如果候选过多，还要进入 `top_m`

也就是说：

- `资源足够` 只是必要条件，不是充分条件
- 当前节点没有“保底直通”逻辑

### 5.3 当前节点即使在候选集合里，也可能根本没被探测到

因为推理侧最多只探测 10 个候选。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L262)

如果：

- 当前节点在候选集合里
- 但它在候选列表中的排序位置靠后
- 而 actor 的首选附近前 10 个候选都失败了

那么当前节点也不会被真正尝试。

---

## 6. 为什么会显示“无候选节点集合”

这一点最容易误解。

### 6.1 “无候选节点”发生在 actor 之前

触发条件是：

- `candidates.empty()`

即启发式裁剪器返回空集合。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L206)

因此它的根因只能来自以下几类：

1. 没有资源足够的节点
2. 节点虽然资源足够，但从 `prev_node` 不可达
3. 节点虽然资源足够，但到 `dest_node` 不可达
4. 所有可达节点都满足不了 `d1 + d2 <= remaining_delay`
5. 图索引转换失败（极少见）

请注意：

- `无候选节点` 不是由 actor 概率造成的
- `无候选节点` 也不是由第二阶段带宽/可靠性校验直接造成的

### 6.2 一个真实样例：`sfc_2`

我用现有结果文件复盘了一个实际的空候选样例：

- 请求：`sfc_2`
- 失败点：`vnf_2_3`
- 失败前当前节点：`SAT_003_121`
- 当前节点资源：足够
- 当前节点到目的节点的最短时延：`6.257 ms`
- 剩余时延预算：`5.613 ms`

这意味着：

- 虽然当前节点资源足够
- 但即使把这个 VNF 继续放在当前节点，后续到目的节点的最低时延都已经超过剩余预算
- 所以当前节点也必须被裁掉
- 最终候选集合为空

这个现象说明：

> “当前节点资源足够” 不代表 “当前节点仍满足整条链路的时延可行性”。

---

## 7. 为什么明明存在可部署方案，却显示“无候选”或最后失败

这里实际上有两种完全不同的失败。

### 7.1 失败类型 A：`No candidate nodes after heuristic pruning`

这是第一阶段失败。

代表：

- 在“资源 + 连通 + 剩余时延”这个筛法下，一个节点都没留下

如果你肉眼觉得“应该还能部署”，通常是以下几种情况之一：

1. 你只看到了当前节点资源足够，但没把“当前节点到最终目的节点的剩余时延”算进去
2. 你看到了某个节点理论可放，但它到目的节点的最短路已经超预算
3. 你看到的是“某条非最短但更合理的未来路径”，而当前裁剪器只看 `最短时延 d1+d2`
4. 你把“中间 VNF 放得下”理解成“整条 SFC 还可行”，但裁剪器要求当前步骤已经要保住通往最终目的地的时延下界

### 7.2 失败类型 B：`No feasible node/path in fallback probes`

这是第二阶段失败。

代表：

- 候选集合其实不为空
- actor 也选了一个首选
- 但最多探测的 10 个候选都被后续硬约束拒绝了

这类失败时，经常会出现“明显还有别的可部署点，但程序就是没走到”的感觉。

原因很简单：

- 候选集合可能有很多个
- 但程序只探测其中 10 个
- 没探测到的候选即使可行，也不会被用上

### 7.3 一个真实样例：`sfc_169`

我复盘了 `sfc_169` 的一次失败：

- 失败点：`vnf_169_3`
- 当时候选数：`80`
- actor 首选节点：`SAT_000_025`
- 当前节点：`SAT_002_079`
- 当前节点资源：足够

进一步重构当时的候选排序后发现：

- actor 首选在候选排序中的位置约为第 `25`
- 当前节点在候选排序中的位置约为第 `28`
- 推理器只探测从 actor 首选开始的前 `10` 个候选
- 当前节点不在这 10 个里

因此：

> 这次不是“当前节点不合法”，而是“当前节点没有被探测到”。

同时，这次已记录的尝试里，多个候选都是因为 `Projected reliability below SLA` 被拒绝，说明第二阶段硬可靠性前瞻校验比第一阶段裁剪更严格。

---

## 8. 为什么 actor 概率最高的策略并非最优

这是当前架构的必然结果，不是单点 bug。

### 8.1 actor 只在候选子集上比较，不在全图比较

actor 的 softmax 只覆盖 `candidate_indices` 中的节点。[drl_agent.py](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/ground_training/models/drl_agent.py#L27)

所以“最高概率”真正含义是：

> 在启发式裁剪保留下来的这批候选里，actor 最偏好哪个。

它不是：

> 全图全约束下的全局最优节点。

### 8.2 actor 的输入不包含每个候选的显式精确路径校验结果

actor 输入的是：

- 候选节点嵌入
- 当前 VNF 资源需求
- 一小部分上下文标量

推理上下文只填了前 7 个维度，很多 48 维上下文实际为 0。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L235)

它没有直接看到每个候选的：

- 当前精确最短路径
- 路径 hop 数
- 路径带宽瓶颈
- 候选部署后的最终收尾路径代价
- 完整未来多 VNF 组合可行性

所以 actor 本质上是在做近似决策，不是精确求解器。

### 8.3 推理侧真正裁决的是“actor + 硬约束验证器”

真正决定能否落地的不是 actor，而是后面的 validator：

- 资源检查
- 带宽路径检查
- 时延检查
- 可靠性前瞻检查

对应代码：[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L268)

因此很常见的情况是：

1. actor 给某候选最高概率
2. 该候选被硬约束拒绝
3. 回退尝试别的候选
4. 某个非最高概率候选反而成功

这并不矛盾。

---

## 9. 为什么会出现很多超长链路（60 跳以上）

这个现象与当前路径计算目标函数直接相关。

### 9.1 路径搜索只优化“总时延”，不限制 hop 数

无论是到候选节点的路径，还是最后到目的节点的收尾路径，都是 Dijkstra 最短时延搜索。[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L31)

当前没有：

- hop 上限
- hop 惩罚
- 绕路惩罚
- path stretch 惩罚

所以只要很多短边累计时延仍然更低，系统就会接受一条很长的多跳路径。

### 9.2 可靠性采用几何平均，不是链路乘积

路径可靠性实现用了几何平均：

- `exp(sum(log(rel)) / hops)`

见：[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L124)

这会显著减轻长路径的可靠性惩罚。直观上：

- 如果每条边可靠性都接近 0.98
- 真实链路串联乘积会随着 hop 数快速下降
- 但几何平均基本仍接近单跳平均可靠性

这会让长路径在可靠性上显得“没那么差”。

### 9.3 结果文件中的实际统计

我对 [final_results.json](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/results/final_results.json) 做了统计：

- 共 `5000` 个 SFC 请求结果
- 已记录路径段中，`>= 60 hop` 的路径段有 `6484` 条
- 观测到的最大路径长度达到 `168 hop`

这说明长路径不是个别偶发，而是当前目标函数自然产生的结果。

---

## 10. 训练侧与推理侧存在的关键口径差异

这一部分很重要，因为它直接解释“训练看起来学到了，推理却不稳定”的原因。

### 10.1 训练时每一步都重新计算图嵌入，推理时不是

训练时，每一步都会基于当前资源/带宽已经变化后的图重新生成节点特征和 GNN 嵌入。[trainer.py](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/ground_training/training/trainer.py#L253)

推理时，GNN 嵌入在每个拓扑上只计算一次，然后复用到该拓扑的全部请求、全部 VNF 步骤。[main.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/main.cpp#L293)

这意味着：

- 推理侧 actor 看到的节点嵌入并没有实时反映当前请求内已经消耗掉的资源和带宽
- 但训练时 actor 是在“动态更新后的图表示”上学策略

这是一个很明显的 train/infer mismatch。

### 10.2 训练启发式裁剪与推理启发式裁剪不完全一致

训练侧启发式裁剪：

- 主要按 `d1 + d2` 排序
- 只遍历最多 `1200` 个节点
- 收到 `top_m * 3` 个候选后就提前停止

见：[train.py](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/ground_training/train.py#L41)

推理侧启发式裁剪：

- 会遍历全图节点
- 使用 `资源利用率 + 时延` 混合分数排序
- 再截断到 `top_m`

见：[heuristic_pruner.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/heuristic_pruner.cpp#L57)

这意味着：

- 训练时 actor 学到的候选排序空间
- 和推理时真正收到的候选集合

并不是同一种分布。

### 10.3 训练默认不做严格可靠性硬剪枝，推理会做

训练环境里，默认只有在 `strict_reliability=True` 且到最后一个 VNF 或最终收尾时，才做硬可靠性失败判定。[sfc_env.py](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/ground_training/environment/sfc_env.py#L229) [sfc_env.py](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/ground_training/environment/sfc_env.py#L331)

推理时则在每一步候选探测时都会做：

- `next_reliability * optimistic_future_rel >= requirement`

见：[sfc_orchestrator.cpp](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/onboard_inference/src/sfc_orchestrator.cpp#L314)

这会导致：

- 训练时某些动作被认为“还能继续尝试”
- 推理时却在当前步直接被判死

---

## 11. 现有结果文件反映出的主要失败模式

我对 [final_results.json](/Users/t1an/Desktop/project/SFC/sfc_deploy/train/results/final_results.json) 做了统计，得到：

- 失败最多的是 `Final latency exceeded`：`474`
- 其次是 `Final reliability not met`：`208`
- VNF 级轨迹中，`No candidate nodes after heuristic pruning`：`422`
- VNF 级轨迹中，`No feasible node/path in fallback probes`：`57`

这说明系统的主要问题不是 actor 完全不会选，而是：

1. 时延预算经常在前面步骤已经被压缩得过紧
2. 候选裁剪与硬约束验证之间存在明显断层
3. 最后收尾到目的节点时经常超时或失去可靠性

---

## 12. 对你提出问题的逐条回答

### Q1：actor 网络的作用是什么？

答：在启发式裁剪后的候选集合内部做排序，输出一个概率分布，选出“最值得优先尝试”的候选节点。它不是全局求解器，也不生成候选集合。

### Q2：候选节点集合怎么生成？

答：先算 `prev -> node` 和 `node -> dest` 的最短时延；再过滤掉资源不足、不可达、或 `d1 + d2 > remaining_delay` 的节点；最后按启发式分数排序，截取前 `top_m`。

### Q3：为什么有时明明存在方案，却显示无候选节点？

答：因为你看到的“存在方案”通常只说明“某节点资源还够”，但裁剪器要求更严格：它必须同时满足到最终目的节点的剩余时延下界。当前节点资源够，不代表 `当前节点 -> 目的节点` 还来得及。

### Q4：为什么当前节点明明资源足够，却没有被选？

答：有三种可能：

1. 当前节点到目的节点的最短时延已经超过剩余预算，所以根本进不了候选集合。
2. 当前节点进了候选集合，但没排进 `top_m`。
3. 当前节点在候选集合里，但推理只探测 10 个候选，它没被探测到。

### Q5：为什么概率最高的策略并非最优？

答：因为概率最高只是在“当前候选子集 + 当前模型认知”下的偏好最高，不等于当前实例上的全局最优，更不等于能通过后续硬约束验证。

### Q6：为什么会有很多 60 跳以上长链路？

答：因为路径规划目标只最小化时延，没有 hop 惩罚，而路径可靠性又用了几何平均，长路径不会被强烈惩罚，所以大量长路径会自然出现。

---

## 13. 你现在最需要知道的三个核心问题

### 13.1 “无候选节点”并不等于“真的没有任何节点能放”

它只等于：

> 在当前启发式裁剪口径下，没有节点同时满足资源、可达性和剩余时延下界。

### 13.2 “actor 选错了”不是唯一问题

更大的问题其实是：

- 候选集合口径本身过硬
- 回退探测只看 10 个
- 训练与推理口径不一致
- 推理图嵌入还是静态的

### 13.3 当前节点虽然支持原地部署，但没有保底机制

这是你观察到“明明当前节点还能放，程序却放弃”的根本原因之一。

---

## 14. 如果要让行为更符合你的直觉，应优先改哪几处

如果后续要改代码，我建议优先检查这 4 个点：

1. 给 `prev_node` 加一个强制保底候选逻辑，只要资源足够且未违反硬约束，就强行加入候选集合。
2. 取消“最多只试 10 个候选”的硬限制，或者至少在失败时补试 `prev_node`。
3. 推理侧像训练侧一样，在每个 VNF 步骤后重新计算图特征和 GNN 嵌入。
4. 给路径搜索加入 hop 惩罚或 hop 上限，并重新审视几何平均可靠性的设计。

如果你需要，我下一步可以继续直接做两件事之一：

1. 把推理引擎改成“当前节点保底 + 失败时补探测当前节点”的版本。
2. 把推理日志增强为“输出完整候选列表、每个候选的概率、排名、拒绝原因”的可视化调试版本。
