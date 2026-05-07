"""open5gs core-network request generator.

The training task is no longer a variable-length SFC chain with ingress and
egress nodes.  Every request represents one full open5gs 5GC deployment over a
static satellite constellation.
"""

from __future__ import annotations

import json
import random
from typing import Dict, List

import numpy as np

try:
    from training.open5gs_profile import (
        BUSINESS_DIMENSIONS,
        CORE_NF_RESOURCE_RANGES,
        CORE_NF_TYPES,
        CORE_NF_BUSINESS_PROFILES,
        CORE_NF_FEATURE_DIM,
        CONTEXT_FEATURE_DIM,
        NODE_FEATURE_DIM,
        dependencies_for_request,
        nf_profile,
        nf_role,
    )
except ImportError:  # pragma: no cover - direct script execution from train/training
    from open5gs_profile import (
        BUSINESS_DIMENSIONS,
        CORE_NF_RESOURCE_RANGES,
        CORE_NF_TYPES,
        CORE_NF_BUSINESS_PROFILES,
        CORE_NF_FEATURE_DIM,
        CONTEXT_FEATURE_DIM,
        NODE_FEATURE_DIM,
        dependencies_for_request,
        nf_profile,
        nf_role,
    )


def _round_business(load: Dict[str, float]) -> Dict[str, float]:
    return {dim: round(float(load.get(dim, 0.0)), 5) for dim in BUSINESS_DIMENSIONS}


def _sample_request_business_demand() -> Dict[str, float]:
    """Sample one request-level business demand without traffic tiers."""
    base = float(np.random.uniform(0.55, 1.15))
    shape = {
        "signaling_load": np.random.uniform(0.85, 1.20),
        "session_load": np.random.uniform(0.75, 1.15),
        "user_plane_load": np.random.uniform(0.70, 1.25),
        "mobility_load": np.random.uniform(0.65, 1.10),
        "policy_load": np.random.uniform(0.55, 1.05),
        "auth_load": np.random.uniform(0.50, 1.00),
    }
    return _round_business({dim: min(1.0, base * value) for dim, value in shape.items()})


def _scaled_nf_business_load(nf_type: str, request_demand: Dict[str, float]) -> Dict[str, float]:
    profile = nf_profile(nf_type)
    return _round_business({dim: profile[dim] * request_demand.get(dim, 1.0) for dim in BUSINESS_DIMENSIONS})


def _sample_nf(nf_type: str, request_id: str, request_demand: Dict[str, float]) -> Dict:
    ranges = CORE_NF_RESOURCE_RANGES[nf_type]
    bw_req = float(np.random.uniform(*ranges["bw"]))
    role = nf_role(nf_type)
    return {
        "core_nf_id": f"{request_id}_{nf_type}",
        "name": f"{request_id}_{nf_type}",
        "core_nf_type": nf_type,
        "nf_type": nf_type,
        "nf_role": role,
        "stateful": nf_type not in {"scp", "nrf"},
        "cpu_required": round(float(np.random.uniform(*ranges["cpu"])), 3),
        "mem_required": round(float(np.random.uniform(*ranges["mem"])), 3),
        "disk_required_gb": round(float(np.random.uniform(*ranges["disk"])), 3),
        "bandwidth_required_gbps": round(bw_req, 4),
        "cpu": 0.0,  # filled below for compatibility with existing JSON readers
        "mem": 0.0,
        "disk": 0.0,
        "bw_in": round(bw_req, 4),
        "bw_out": round(bw_req, 4),
        "business_load_profile": _round_business(CORE_NF_BUSINESS_PROFILES[nf_type]),
        "business_load_demand": _scaled_nf_business_load(nf_type, request_demand),
    }


def _finalize_nf_compat_fields(nf: Dict) -> Dict:
    nf["cpu"] = nf["cpu_required"]
    nf["mem"] = nf["mem_required"]
    nf["disk"] = nf["disk_required_gb"]
    return nf


def _dependency_bandwidth(dep: Dict, core_nfs: List[Dict], request_bw: float) -> float:
    nf_by_type = {nf["nf_type"]: nf for nf in core_nfs}
    src_bw = float(nf_by_type[dep["source"]]["bandwidth_required_gbps"])
    dst_bw = float(nf_by_type[dep["target"]]["bandwidth_required_gbps"])
    base = max(src_bw, dst_bw, request_bw * 0.18)
    return round(float(base * float(dep.get("bandwidth_scale", 0.5))), 4)


def build_sfc_requests_payload(
    num_requests,
    node_list,
    topology_data=None,
    load_profile=None,
    seed=None,
    topology_scale=None,
    topology_file=None,
    request_id_prefix="core",
):
    """Generate full open5gs core-network deployment requests."""
    del node_list, topology_data, load_profile
    if seed is not None:
        random.seed(seed)
        np.random.seed(seed)

    requests = []
    for i in range(num_requests):
        request_id = f"{request_id_prefix}_{i}"
        request_business_demand = _sample_request_business_demand()
        core_nfs = [
            _finalize_nf_compat_fields(_sample_nf(nf_type, request_id, request_business_demand))
            for nf_type in CORE_NF_TYPES
        ]

        request_bw = max(float(nf["bandwidth_required_gbps"]) for nf in core_nfs)
        dependencies = dependencies_for_request()
        for dep in dependencies:
            dep["bandwidth_required_gbps"] = _dependency_bandwidth(dep, core_nfs, request_bw)

        latency_budget = float(np.random.uniform(95.0, 150.0))
        reliability_requirement = float(np.random.uniform(0.76, 0.88))
        request = {
            "request_id": request_id,
            "service_type": "open5gs_full_core",
            "network_domain": "open5gs",
            "core_nfs": core_nfs,
            "core_nf_dependencies": dependencies,
            "business_demand": request_business_demand,
            "bandwidth_demand_gbps": round(request_bw, 4),
            "max_latency_ms": round(latency_budget, 3),
            "reliability_requirement": round(reliability_requirement, 5),
            "sla": {
                "latency_requirement_ms": round(latency_budget, 3),
                "bandwidth_demand_gbps": round(request_bw, 4),
                "reliability_requirement": round(reliability_requirement, 5),
                "max_dependency_hops": 16,
                "max_total_dependency_delay_ms": round(latency_budget, 3),
            },
        }
        requests.append(request)

    return {
        "metadata": {
            "num_requests": num_requests,
            "request_schema": "open5gs_full_core_v1",
            "topology_scale": topology_scale,
            "topology_file": topology_file,
            "generation_seed": seed,
            "core_nf_types": CORE_NF_TYPES,
            "core_nf_count": len(CORE_NF_TYPES),
            "core_nf_dependency_count": len(dependencies_for_request()),
            "feature_dims": {
                "node": NODE_FEATURE_DIM,
                "core_nf": CORE_NF_FEATURE_DIM,
                "context": CONTEXT_FEATURE_DIM,
            },
            "business_dimensions": BUSINESS_DIMENSIONS,
        },
        "requests": requests,
    }


def generate_sfc_requests(
    num_requests,
    node_list,
    output_file,
    topology_data=None,
    load_profile=None,
    seed=None,
    topology_scale=None,
    topology_file=None,
    request_id_prefix="core",
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

    print(f"Generated {num_requests} open5gs core requests -> {output_file}")
    return output_file
