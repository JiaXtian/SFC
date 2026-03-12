import json
import heapq
import random

import networkx as nx
import numpy as np


SFC_TEMPLATES = {
    "web_service": {"vnfs": ["firewall", "waf", "load_balancer", "cache"], "priority": "high", "lat_mult": 0.8},
    "secure_comm": {"vnfs": ["firewall", "ids", "encryption", "nat"], "priority": "high", "lat_mult": 0.7},
    "cdn_service": {"vnfs": ["firewall", "cache", "load_balancer", "compression"], "priority": "medium", "lat_mult": 1.0},
    "vpn_service": {"vnfs": ["firewall", "vpn_gateway", "encryption", "nat"], "priority": "high", "lat_mult": 0.9},
    "video_streaming": {"vnfs": ["firewall", "transcoding", "cache", "qos_manager"], "priority": "medium", "lat_mult": 1.2},
    "iot_service": {"vnfs": ["firewall", "api_gateway", "compression", "cache"], "priority": "low", "lat_mult": 1.5},
    "edge_computing": {"vnfs": ["firewall", "load_balancer", "cache", "qos_manager"], "priority": "high", "lat_mult": 0.6},
    "5g_service": {"vnfs": ["firewall", "qos_manager", "load_balancer", "cache"], "priority": "high", "lat_mult": 0.5},
    "blockchain": {"vnfs": ["firewall", "encryption", "load_balancer", "cache"], "priority": "medium", "lat_mult": 1.0},
    "ai_inference": {"vnfs": ["firewall", "load_balancer", "cache", "api_gateway"], "priority": "medium", "lat_mult": 0.9},
    "data_processing": {"vnfs": ["firewall", "compression", "load_balancer", "cache"], "priority": "low", "lat_mult": 1.3},
    "secure_storage": {"vnfs": ["firewall", "encryption", "ddos_protection", "cache"], "priority": "high", "lat_mult": 1.0},
}

PRIORITY_WEIGHT = {"low": 0.8, "medium": 1.0, "high": 1.3}

VNF_CONFIGS = {
    "firewall": {
        "low": {"cpu": (0.3, 0.6), "mem": (0.5, 1.0), "bw": (0.03, 0.06), "disk": (2.0, 8.0)},
        "medium": {"cpu": (0.5, 1.2), "mem": (1.0, 2.5), "bw": (0.05, 0.15), "disk": (6.0, 18.0)},
        "high": {"cpu": (1.0, 2.0), "mem": (2.0, 4.0), "bw": (0.1, 0.3), "disk": (12.0, 28.0)},
    },
    "nat": {
        "low": {"cpu": (0.2, 0.5), "mem": (0.3, 0.8), "bw": (0.05, 0.1), "disk": (1.0, 4.0)},
        "medium": {"cpu": (0.3, 0.8), "mem": (0.5, 1.5), "bw": (0.1, 0.2), "disk": (2.0, 8.0)},
        "high": {"cpu": (0.5, 1.5), "mem": (1.0, 3.0), "bw": (0.15, 0.4), "disk": (4.0, 12.0)},
    },
    "ids": {
        "low": {"cpu": (0.8, 1.5), "mem": (1.5, 3.0), "bw": (0.1, 0.2), "disk": (6.0, 18.0)},
        "medium": {"cpu": (1.0, 2.0), "mem": (2.0, 5.0), "bw": (0.2, 0.4), "disk": (10.0, 28.0)},
        "high": {"cpu": (1.5, 3.5), "mem": (3.0, 8.0), "bw": (0.3, 0.8), "disk": (18.0, 42.0)},
    },
    "load_balancer": {
        "low": {"cpu": (0.3, 0.8), "mem": (0.5, 1.5), "bw": (0.08, 0.15), "disk": (2.0, 8.0)},
        "medium": {"cpu": (0.5, 1.5), "mem": (1.0, 3.0), "bw": (0.15, 0.3), "disk": (4.0, 14.0)},
        "high": {"cpu": (1.0, 2.5), "mem": (2.0, 5.0), "bw": (0.25, 0.5), "disk": (8.0, 20.0)},
    },
    "cache": {
        "low": {"cpu": (0.2, 0.6), "mem": (1.5, 4.0), "bw": (0.1, 0.3), "disk": (16.0, 64.0)},
        "medium": {"cpu": (0.3, 1.0), "mem": (2.0, 8.0), "bw": (0.2, 0.5), "disk": (48.0, 160.0)},
        "high": {"cpu": (0.5, 1.5), "mem": (4.0, 16.0), "bw": (0.3, 1.0), "disk": (128.0, 320.0)},
    },
    "encryption": {
        "low": {"cpu": (1.0, 2.0), "mem": (0.8, 2.0), "bw": (0.05, 0.15), "disk": (2.0, 8.0)},
        "medium": {"cpu": (1.5, 2.5), "mem": (1.0, 2.5), "bw": (0.1, 0.3), "disk": (4.0, 10.0)},
        "high": {"cpu": (2.0, 4.0), "mem": (1.5, 4.0), "bw": (0.15, 0.5), "disk": (6.0, 16.0)},
    },
    "decryption": {
        "low": {"cpu": (1.0, 2.0), "mem": (0.8, 2.0), "bw": (0.05, 0.15), "disk": (2.0, 8.0)},
        "medium": {"cpu": (1.5, 2.5), "mem": (1.0, 2.5), "bw": (0.1, 0.3), "disk": (4.0, 10.0)},
        "high": {"cpu": (2.0, 4.0), "mem": (1.5, 4.0), "bw": (0.15, 0.5), "disk": (6.0, 16.0)},
    },
    "transcoding": {
        "low": {"cpu": (1.5, 2.5), "mem": (2.0, 4.0), "bw": (0.3, 0.6), "disk": (12.0, 36.0)},
        "medium": {"cpu": (2.0, 3.5), "mem": (3.0, 6.0), "bw": (0.5, 1.0), "disk": (24.0, 64.0)},
        "high": {"cpu": (3.0, 5.0), "mem": (4.0, 10.0), "bw": (0.8, 2.0), "disk": (56.0, 160.0)},
    },
    "compression": {
        "low": {"cpu": (0.8, 1.5), "mem": (1.0, 2.5), "bw": (0.15, 0.3), "disk": (3.0, 10.0)},
        "medium": {"cpu": (1.0, 2.0), "mem": (1.5, 3.5), "bw": (0.3, 0.6), "disk": (6.0, 18.0)},
        "high": {"cpu": (1.5, 3.0), "mem": (2.0, 5.0), "bw": (0.4, 1.0), "disk": (12.0, 32.0)},
    },
    "decompression": {
        "low": {"cpu": (0.6, 1.2), "mem": (0.8, 2.0), "bw": (0.1, 0.25), "disk": (3.0, 10.0)},
        "medium": {"cpu": (0.8, 1.8), "mem": (1.0, 2.8), "bw": (0.2, 0.4), "disk": (6.0, 18.0)},
        "high": {"cpu": (1.2, 2.5), "mem": (1.5, 4.0), "bw": (0.3, 0.7), "disk": (12.0, 32.0)},
    },
    "waf": {
        "low": {"cpu": (1.0, 2.0), "mem": (1.5, 3.0), "bw": (0.1, 0.25), "disk": (8.0, 24.0)},
        "medium": {"cpu": (1.5, 2.5), "mem": (2.0, 4.0), "bw": (0.2, 0.4), "disk": (16.0, 40.0)},
        "high": {"cpu": (2.0, 4.0), "mem": (3.0, 6.0), "bw": (0.3, 0.8), "disk": (24.0, 64.0)},
    },
    "ddos_protection": {
        "low": {"cpu": (1.5, 2.5), "mem": (2.5, 5.0), "bw": (0.3, 0.6), "disk": (12.0, 30.0)},
        "medium": {"cpu": (2.0, 3.5), "mem": (3.0, 6.5), "bw": (0.5, 1.0), "disk": (20.0, 48.0)},
        "high": {"cpu": (3.0, 5.0), "mem": (4.0, 10.0), "bw": (0.7, 1.8), "disk": (36.0, 96.0)},
    },
    "qos_manager": {
        "low": {"cpu": (0.3, 0.8), "mem": (0.8, 2.0), "bw": (0.05, 0.12), "disk": (2.0, 6.0)},
        "medium": {"cpu": (0.5, 1.2), "mem": (1.0, 2.5), "bw": (0.1, 0.2), "disk": (4.0, 10.0)},
        "high": {"cpu": (0.8, 2.0), "mem": (1.5, 4.0), "bw": (0.15, 0.4), "disk": (8.0, 16.0)},
    },
    "vpn_gateway": {
        "low": {"cpu": (0.8, 1.5), "mem": (1.5, 3.0), "bw": (0.15, 0.3), "disk": (6.0, 18.0)},
        "medium": {"cpu": (1.0, 2.0), "mem": (2.0, 4.0), "bw": (0.3, 0.6), "disk": (10.0, 26.0)},
        "high": {"cpu": (1.5, 3.0), "mem": (3.0, 6.0), "bw": (0.4, 1.0), "disk": (20.0, 42.0)},
    },
    "api_gateway": {
        "low": {"cpu": (0.6, 1.2), "mem": (1.2, 2.5), "bw": (0.1, 0.2), "disk": (4.0, 12.0)},
        "medium": {"cpu": (0.8, 1.8), "mem": (1.5, 3.5), "bw": (0.2, 0.4), "disk": (8.0, 22.0)},
        "high": {"cpu": (1.2, 2.5), "mem": (2.0, 5.0), "bw": (0.3, 0.6), "disk": (14.0, 36.0)},
    },
}

TARGET_TOTAL_HOPS = 25
PATH_SOFTENING_EXPONENT = 0.46
EXCESS_HOP_RELIABILITY_PENALTY = 0.9988
HOP_PENALTY_MS = 2.5


def _clamp01(value):
    return max(0.0, min(1.0, float(value)))


def _softened_path_reliability(raw_reliability, hops):
    if hops <= 0:
        return 1.0
    raw = max(1e-9, _clamp01(raw_reliability))
    geometric_mean = raw ** (1.0 / max(1, hops))
    softened_product = raw ** PATH_SOFTENING_EXPONENT
    blend = (0.7 * softened_product + 0.3 * geometric_mean) if hops <= 6 else (0.82 * softened_product + 0.18 * geometric_mean)
    excess_hops = max(0, hops - 6)
    return _clamp01(blend * (EXCESS_HOP_RELIABILITY_PENALTY ** excess_hops))


def _estimate_link_reliability(edge):
    status = str(edge.get("status", "active")).lower()
    if status == "down":
        return 0.0
    base = _clamp01(edge.get("link_reliability", edge.get("reliability", 0.98)))
    bw_total = float(edge.get("bandwidth_gbps", 0.0))
    bw_avail = float(edge.get("bandwidth_available_gbps", 0.0))
    bw_ratio = _clamp01(bw_avail / bw_total) if bw_total > 1e-9 else 0.0
    bandwidth_factor = 0.98 + 0.02 * bw_ratio
    status_penalty = 0.985 if status == "congested" else 1.0
    return _clamp01(base * bandwidth_factor * status_penalty)


def _build_feasibility_graph(topology_data):
    if not topology_data:
        return None
    topo = topology_data.get("topology", topology_data)
    nodes = topo.get("nodes", [])
    links = topo.get("links", [])
    if not nodes or not links:
        return None

    G = nx.DiGraph()
    for node in nodes:
        status = str(node.get("status", "active")).lower()
        if status in {"down", "inactive", "failed"}:
            continue
        node_id = node.get("id")
        if node_id:
            G.add_node(node_id)

    def _merge_edge(u, v, attrs):
        if u not in G or v not in G:
            return
        if not G.has_edge(u, v):
            G.add_edge(u, v, **attrs)
            return
        cur = G[u][v]
        cur["latency_ms"] = min(float(cur.get("latency_ms", 1.0)), float(attrs.get("latency_ms", 1.0)))
        cur["bandwidth_gbps"] = max(float(cur.get("bandwidth_gbps", 0.0)), float(attrs.get("bandwidth_gbps", 0.0)))
        cur["bandwidth_available_gbps"] = max(float(cur.get("bandwidth_available_gbps", 0.0)), float(attrs.get("bandwidth_available_gbps", 0.0)))
        cur["status"] = "active" if str(cur.get("status", "active")).lower() != "down" else str(attrs.get("status", "active"))
        cur["link_reliability"] = max(float(cur.get("link_reliability", 0.0)), float(attrs.get("link_reliability", 0.0)))

    for link in links:
        raw_status = link.get("link_status")
        if raw_status is None:
            status_text = str(link.get("status", "active")).lower()
            is_up = status_text in {"active", "up", "healthy", "congested"}
        else:
            is_up = int(raw_status) == 1
        if not is_up:
            continue

        src = link.get("source")
        dst = link.get("target")
        if not src or not dst or src == dst:
            continue
        bw_total = float(link.get("bandwidth_gbps", 0.0))
        bw_avail = float(link.get("bandwidth_available_gbps", bw_total))
        if bw_avail <= 1e-9:
            continue

        attrs = {
            "latency_ms": float(link.get("latency_ms", 1.0)),
            "bandwidth_gbps": bw_total,
            "bandwidth_available_gbps": bw_avail,
            "status": str(link.get("status", "active")),
            "link_reliability": float(link.get("link_reliability", link.get("reliability", 0.98))),
        }
        _merge_edge(src, dst, attrs)
        _merge_edge(dst, src, attrs)
    return G


def _find_constrained_path(graph, src, dst, bw_req, max_hops=TARGET_TOTAL_HOPS):
    if graph is None or src not in graph or dst not in graph:
        return [], float("inf"), 0.0, 0, 0.0
    if src == dst:
        return [src], 0.0, 1.0, 0, float("inf")

    dist = {src: 0.0}
    latency = {src: 0.0}
    hops = {src: 0}
    reliability_raw = {src: 1.0}
    bottleneck = {src: float("inf")}
    prev = {}
    pq = [(0.0, src)]

    while pq:
        cur_cost, u = heapq.heappop(pq)
        if cur_cost > dist.get(u, float("inf")) + 1e-9:
            continue
        if u == dst:
            break
        for v, edge in graph[u].items():
            if float(edge.get("bandwidth_available_gbps", 0.0)) + 1e-9 < float(bw_req):
                continue
            next_hops = hops[u] + 1
            if max_hops > 0 and next_hops > max_hops:
                continue
            next_latency = latency[u] + float(edge.get("latency_ms", 0.0))
            next_cost = next_latency + HOP_PENALTY_MS * next_hops
            edge_rel = max(1e-9, _estimate_link_reliability(edge))
            next_rel = reliability_raw[u] * edge_rel
            next_bottleneck = min(bottleneck[u], float(edge.get("bandwidth_available_gbps", 0.0)))

            old_cost = dist.get(v, float("inf"))
            old_latency = latency.get(v, float("inf"))
            old_rel = reliability_raw.get(v, 0.0)
            if (
                next_cost + 1e-9 < old_cost
                or (
                    abs(next_cost - old_cost) <= 1e-9
                    and (
                        next_latency + 1e-9 < old_latency
                        or (abs(next_latency - old_latency) <= 1e-9 and next_rel > old_rel + 1e-9)
                    )
                )
            ):
                dist[v] = next_cost
                latency[v] = next_latency
                hops[v] = next_hops
                reliability_raw[v] = next_rel
                bottleneck[v] = next_bottleneck
                prev[v] = u
                heapq.heappush(pq, (next_cost, v))

    if dst not in prev:
        return [], float("inf"), 0.0, 0, 0.0

    path = [dst]
    while path[-1] != src:
        p = prev.get(path[-1])
        if p is None:
            return [], float("inf"), 0.0, 0, 0.0
        path.append(p)
    path.reverse()
    hop_count = max(0, len(path) - 1)
    rel = _softened_path_reliability(reliability_raw.get(dst, 1.0), hop_count)
    return path, float(latency.get(dst, 0.0)), float(rel), hop_count, float(bottleneck.get(dst, 0.0))


def _pick_src_dst_with_sla(
    node_list,
    graph,
    latency_budget,
    bandwidth_demand,
    reliability_requirement,
    hop_cap=TARGET_TOTAL_HOPS,
    max_attempts=120,
):
    if len(node_list) < 2:
        return None
    if graph is None:
        src, dst = random.sample(node_list, 2)
        return src, dst, latency_budget, bandwidth_demand, reliability_requirement

    candidate_nodes = [n for n in node_list if n in graph]
    if len(candidate_nodes) < 2:
        src, dst = random.sample(node_list, 2)
        return src, dst, latency_budget, bandwidth_demand, reliability_requirement

    hop_cap = int(max(2, min(TARGET_TOTAL_HOPS, hop_cap)))
    hop_caps = sorted({hop_cap, min(TARGET_TOTAL_HOPS, hop_cap + 2), min(TARGET_TOTAL_HOPS, hop_cap + 4)})
    tries_per_cap = max(14, max_attempts // max(1, len(hop_caps)))

    best = None
    for cap in hop_caps:
        for _ in range(tries_per_cap):
            src, dst = random.sample(candidate_nodes, 2)
            path, delay, rel, hops, bottleneck_bw = _find_constrained_path(
                graph,
                src,
                dst,
                bw_req=bandwidth_demand,
                max_hops=cap,
            )
            if not path:
                continue
            if delay <= latency_budget and rel >= reliability_requirement:
                return src, dst, latency_budget, bandwidth_demand, reliability_requirement

            score = (
                delay / max(1e-6, latency_budget)
                + max(0.0, reliability_requirement - rel) * 6.0
                + 0.22 * max(0, hops - cap)
                + 0.045 * hops
            )
            if best is None or score < best["score"]:
                best = {
                    "src": src,
                    "dst": dst,
                    "delay": delay,
                    "rel": rel,
                    "hops": hops,
                    "bottleneck": bottleneck_bw,
                    "score": score,
                }

    if best is not None:
        adjusted_latency = max(latency_budget, best["delay"] * random.uniform(1.18, 1.45))
        adjusted_bw = min(
            bandwidth_demand,
            max(0.02, best["bottleneck"] * random.uniform(0.72, 0.94)),
        )
        adjusted_rel = min(
            reliability_requirement,
            max(0.58, best["rel"] * random.uniform(0.92, 0.98)),
        )
        return best["src"], best["dst"], adjusted_latency, adjusted_bw, adjusted_rel

    # 兜底：至少保证在目标跳数内可达，再适度放宽SLA，避免生成天然无解请求。
    fallback_attempts = max(20, max_attempts // 2)
    for _ in range(fallback_attempts):
        src, dst = random.sample(candidate_nodes, 2)
        path, delay, rel, _hops, bottleneck_bw = _find_constrained_path(
            graph,
            src,
            dst,
            bw_req=0.0,
            max_hops=TARGET_TOTAL_HOPS,
        )
        if not path:
            continue
        adjusted_latency = max(latency_budget, delay * random.uniform(1.22, 1.55))
        adjusted_bw = min(
            bandwidth_demand,
            max(0.02, bottleneck_bw * random.uniform(0.68, 0.92)),
        )
        adjusted_rel = min(
            reliability_requirement,
            max(0.58, rel * random.uniform(0.90, 0.97)),
        )
        return src, dst, adjusted_latency, adjusted_bw, adjusted_rel

    src, dst = random.sample(candidate_nodes, 2)
    return src, dst, max(latency_budget, 120.0), min(bandwidth_demand, 0.12), min(reliability_requirement, 0.72)


def _sample_load_level(load_profile):
    if load_profile == "mixed":
        return random.choices(["low", "medium", "high"], weights=[0.25, 0.5, 0.25])[0]
    return load_profile


def _sample_core_network_load(load_level):
    ranges = {
        "low": (0.12, 0.38),
        "medium": (0.28, 0.62),
        "high": (0.48, 0.86),
    }
    low, high = ranges.get(load_level, (0.2, 0.9))
    return float(np.random.uniform(low, high))


def _sample_reliability_requirement(priority, load_level):
    # 端到端路径在多跳卫星网络中会产生乘法衰减，训练阶段把目标设在可达范围，
    # 否则策略几乎永远无法成功。
    base = {"low": 0.68, "medium": 0.75, "high": 0.82}.get(priority, 0.75)
    if load_level == "high":
        base += 0.015
    return float(min(0.90, max(0.58, base + np.random.uniform(-0.035, 0.035))))


def build_sfc_requests_payload(
    num_requests,
    node_list,
    topology_data=None,
    load_profile="mixed",
    seed=None,
    topology_scale=None,
    topology_file=None,
    request_id_prefix="sfc",
):
    """生成SFC请求（含 SLA、业务负载、可靠性、磁盘需求）。"""
    if seed is not None:
        random.seed(seed)
        np.random.seed(seed)
    feasibility_graph = _build_feasibility_graph(topology_data)

    requests = []
    service_types = list(SFC_TEMPLATES.keys())

    for i in range(num_requests):
        service_type = random.choice(service_types)
        template = SFC_TEMPLATES[service_type]
        vnf_types = template["vnfs"]
        load_level = _sample_load_level(load_profile)
        core_network_load = _sample_core_network_load(load_level)

        vnf_sequence = []
        total_vnf_bw = 0.0
        for j, vnf_type in enumerate(vnf_types):
            config = VNF_CONFIGS.get(vnf_type, VNF_CONFIGS["firewall"])[load_level]
            bw_req = float(np.random.uniform(*config["bw"]))

            vnf = {
                "vnf_id": f"vnf_{i}_{j}",
                "vnf_type": vnf_type,
                "cpu_required": round(float(np.random.uniform(*config["cpu"])), 2),
                "mem_required": round(float(np.random.uniform(*config["mem"])), 2),
                "disk_required_gb": round(float(np.random.uniform(*config["disk"])), 2),
                "bandwidth_required_gbps": round(bw_req, 3),
            }
            total_vnf_bw += bw_req
            vnf_sequence.append(vnf)

        base_latency = len(vnf_types) * 14.0
        load_penalty = 1.0 + 0.5 * core_network_load
        latency_budget = (
            base_latency
            * template["lat_mult"]
            * load_penalty
            * float(np.random.uniform(1.15, 1.55))
        )

        bandwidth_demand = max(0.04, total_vnf_bw * float(np.random.uniform(0.22, 0.45)))
        reliability_requirement = _sample_reliability_requirement(template["priority"], load_level)
        reserved_chain_hops = int(np.clip(np.round(len(vnf_types) * np.random.uniform(2.0, 2.8)), 6, 16))
        src_dst_hop_cap = max(4, min(TARGET_TOTAL_HOPS - 2, TARGET_TOTAL_HOPS - reserved_chain_hops))
        picked = _pick_src_dst_with_sla(
            node_list=node_list,
            graph=feasibility_graph,
            latency_budget=float(latency_budget),
            bandwidth_demand=float(bandwidth_demand),
            reliability_requirement=float(reliability_requirement),
            hop_cap=src_dst_hop_cap,
        )
        if picked is None:
            if len(node_list) < 2:
                continue
            src, dst = random.sample(node_list, 2)
        else:
            src, dst, latency_budget, bandwidth_demand, reliability_requirement = picked

        request = {
            "request_id": f"{request_id_prefix}_{i}",
            "service_type": service_type,
            "vnf_sequence": vnf_sequence,
            "source_node": src,
            "destination_node": dst,
            "max_latency_ms": round(latency_budget, 2),
            "priority": template["priority"],
            "priority_weight": PRIORITY_WEIGHT[template["priority"]],
            "load_level": load_level,
            "core_network_load": round(core_network_load, 4),
            "bandwidth_demand_gbps": round(float(bandwidth_demand), 3),
            "reliability_requirement": round(reliability_requirement, 5),
            "max_total_hops": 25,
            "hard_max_total_hops": 30,
            "sla": {
                "bandwidth_demand_gbps": round(float(bandwidth_demand), 3),
                "latency_requirement_ms": round(latency_budget, 2),
                "reliability_requirement": round(reliability_requirement, 5),
                "max_total_hops": 25,
            },
        }
        requests.append(request)

    payload = {
        "metadata": {
            "num_requests": num_requests,
            "load_profile": load_profile,
            "topology_scale": topology_scale,
            "topology_file": topology_file,
            "service_types": service_types,
            "generation_seed": seed,
            "schema": {
                "vnf_extra_fields": ["disk_required_gb"],
                "request_extra_fields": [
                    "core_network_load",
                    "bandwidth_demand_gbps",
                    "reliability_requirement",
                    "sla",
                ],
            },
        },
        "requests": requests,
    }
    return payload


def generate_sfc_requests(
    num_requests,
    node_list,
    output_file,
    topology_data=None,
    load_profile="mixed",
    seed=None,
    topology_scale=None,
    topology_file=None,
    request_id_prefix="sfc",
):
    payload = build_sfc_requests_payload(
        num_requests=num_requests,
        node_list=node_list,
        topology_data=topology_data,
        load_profile=load_profile,
        seed=seed,
        topology_scale=topology_scale,
        topology_file=topology_file,
        request_id_prefix=request_id_prefix,
    )

    with open(output_file, "w") as f:
        json.dump(payload, f, indent=2)

    print(f"Generated {num_requests} requests ({load_profile}) -> {output_file}")
    return output_file
