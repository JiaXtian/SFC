const EN_TO_ZH_RULES: Array<[RegExp, string]> = [
  [/No path to destination node/gi, '无法到达宿节点（当前拓扑中无可用路径）'],
  [/No path for core NF dependency/gi, '核心网依赖路径不可达'],
  [/Core NF dependency endpoint not placed/gi, '核心网依赖端网元未完成放置'],
  [/Core NF dependency path violates bandwidth\/status/gi, '核心网依赖路径带宽或链路状态不满足要求'],
  [/Aggregate link bandwidth insufficient|aggregate_bandwidth_insufficient|aggregate_insufficient_bandwidth/gi, '多条依赖边复用同一链路后聚合带宽不足'],
  [/Aggregate link budget violates status|aggregate_link_down/gi, '依赖路径包含不可用链路'],
  [/All generated candidates violate hard constraints/gi, '所有候选方案均不满足硬性约束'],
  [/No hard-constraint-feasible candidate returned by inference engine/gi, '推理引擎未返回满足硬性约束的候选方案'],
  [/Candidate violates hard deployment constraints/gi, '候选方案不满足硬性部署约束'],
  [/Candidate no longer satisfies current resource constraints/gi, '候选方案已不满足当前资源约束'],
  [/Registration SLA path unreachable/gi, '注册 SLA 路径不可达'],
  [/PDU Session SLA path unreachable/gi, 'PDU Session SLA 路径不可达'],
  [/Registration latency SLA violated|registration_latency_exceeded/gi, '注册时延 SLA 超限'],
  [/PDU Session latency SLA violated|pdu_session_latency_exceeded/gi, 'PDU Session 时延 SLA 超限'],
  [/No feasible deployment found/gi, '未找到可部署方案'],
  [/All generated candidates violate constraints/gi, '所有候选方案均不满足约束'],
  [/source_node and destination_node must be different/gi, '旧版流量端点配置不合法'],
  [/source_node and destination_node are required/gi, '旧版流量端点字段缺失'],
  [/source_node or destination_node not found in current topology/gi, '旧版流量端点不在当前拓扑中'],
  [/Failed to allocate resources/gi, '资源分配失败'],
  [/Invalid JSON/gi, '请求格式错误'],
  [/Invalid SFC request/gi, '核心网请求参数非法'],
  [/Deployment rolled back/gi, '部署已回滚'],
  [/Deployment not found/gi, '未找到该部署'],
  [/topology.*stale/gi, '候选方案对应的拓扑版本已过期，请重新生成方案'],
]

export function toChineseFailureText(input: unknown): string {
  const raw = String(input ?? '').trim()
  if (!raw) return ''
  let out = raw
  EN_TO_ZH_RULES.forEach(([re, zh]) => {
    out = out.replace(re, zh)
  })
  return out
}

export function toChineseFailureList(input: unknown): string[] {
  if (Array.isArray(input)) {
    return input
      .map((x) => toChineseFailureText(x))
      .filter(Boolean)
  }
  const single = toChineseFailureText(input)
  if (!single) return []
  return single
    .split('\n')
    .map((x) => x.trim())
    .filter(Boolean)
}
