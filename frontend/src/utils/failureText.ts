const EN_TO_ZH_RULES: Array<[RegExp, string]> = [
  [/No path to destination node/gi, '无法到达宿节点（当前拓扑中无可用路径）'],
  [/No feasible deployment found/gi, '未找到可部署方案'],
  [/All generated candidates violate constraints/gi, '所有候选方案均不满足约束'],
  [/source_node and destination_node must be different/gi, '源节点与宿节点不能相同'],
  [/source_node and destination_node are required/gi, '必须填写源节点与宿节点'],
  [/source_node or destination_node not found in current topology/gi, '源节点或宿节点不在当前拓扑中'],
  [/Failed to allocate resources/gi, '资源分配失败'],
  [/Invalid JSON/gi, '请求格式错误'],
  [/Invalid SFC request/gi, 'SFC 请求参数非法'],
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
