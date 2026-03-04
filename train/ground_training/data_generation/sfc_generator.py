import json
import random

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
    return float(min(0.92, max(0.58, base + np.random.uniform(-0.035, 0.035))))


def generate_sfc_requests(
    num_requests,
    node_list,
    output_file,
    load_profile="mixed",
    seed=None,
    topology_scale=None,
    topology_file=None,
):
    """生成SFC请求（含 SLA、业务负载、可靠性、磁盘需求）。"""
    if seed is not None:
        random.seed(seed)
        np.random.seed(seed)

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

        src, dst = random.sample(node_list, 2)

        base_latency = len(vnf_types) * 14.0
        load_penalty = 1.0 + 0.5 * core_network_load
        latency_budget = (
            base_latency
            * template["lat_mult"]
            * load_penalty
            * float(np.random.uniform(1.15, 1.55))
        )

        bandwidth_demand = max(0.05, total_vnf_bw * float(np.random.uniform(0.28, 0.55)))
        reliability_requirement = _sample_reliability_requirement(template["priority"], load_level)

        request = {
            "request_id": f"sfc_{i}",
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
            "sla": {
                "bandwidth_demand_gbps": round(float(bandwidth_demand), 3),
                "latency_requirement_ms": round(latency_budget, 2),
                "reliability_requirement": round(reliability_requirement, 5),
            },
        }
        requests.append(request)

    output_data = {
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

    with open(output_file, "w") as f:
        json.dump(output_data, f, indent=2)

    print(f"Generated {num_requests} requests ({load_profile}) -> {output_file}")
    return output_file
