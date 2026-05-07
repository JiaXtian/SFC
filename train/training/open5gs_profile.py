"""Shared open5gs core-network deployment profile."""

from __future__ import annotations

from typing import Dict, List


BUSINESS_DIMENSIONS = [
    "signaling_load",
    "session_load",
    "user_plane_load",
    "mobility_load",
    "policy_load",
    "auth_load",
]

CORE_NF_TYPES = [
    "nrf",
    "scp",
    "sepp",
    "amf",
    "smf",
    "upf",
    "ausf",
    "udm",
    "udr",
    "pcf",
    "nssf",
    "bsf",
]

CORE_NF_INDEX = {nf_type: idx for idx, nf_type in enumerate(CORE_NF_TYPES)}

NODE_FEATURE_DIM = 18
CORE_NF_FEATURE_DIM = 24
CONTEXT_FEATURE_DIM = 32
GNN_EMBEDDING_DIM = 192

CORE_NF_RESOURCE_RANGES = {
    "nrf": {"cpu": (0.45, 0.90), "mem": (0.9, 1.8), "disk": (4.0, 8.0), "bw": (0.04, 0.10)},
    "scp": {"cpu": (0.70, 1.25), "mem": (1.2, 2.4), "disk": (5.0, 10.0), "bw": (0.08, 0.18)},
    "sepp": {"cpu": (0.65, 1.20), "mem": (1.1, 2.2), "disk": (5.0, 10.0), "bw": (0.06, 0.15)},
    "amf": {"cpu": (0.95, 1.70), "mem": (1.8, 3.2), "disk": (6.0, 12.0), "bw": (0.10, 0.24)},
    "smf": {"cpu": (1.10, 1.95), "mem": (2.0, 3.6), "disk": (7.0, 14.0), "bw": (0.14, 0.30)},
    "upf": {"cpu": (1.80, 3.20), "mem": (2.8, 5.2), "disk": (10.0, 22.0), "bw": (0.45, 1.20)},
    "ausf": {"cpu": (0.55, 1.05), "mem": (1.0, 2.0), "disk": (4.0, 9.0), "bw": (0.04, 0.10)},
    "udm": {"cpu": (0.70, 1.30), "mem": (1.5, 2.8), "disk": (8.0, 18.0), "bw": (0.05, 0.12)},
    "udr": {"cpu": (0.75, 1.40), "mem": (1.8, 3.2), "disk": (14.0, 28.0), "bw": (0.05, 0.12)},
    "pcf": {"cpu": (0.65, 1.25), "mem": (1.4, 2.6), "disk": (5.0, 12.0), "bw": (0.05, 0.13)},
    "nssf": {"cpu": (0.45, 0.90), "mem": (0.9, 1.8), "disk": (4.0, 8.0), "bw": (0.03, 0.08)},
    "bsf": {"cpu": (0.50, 1.00), "mem": (1.0, 2.0), "disk": (4.0, 9.0), "bw": (0.04, 0.10)},
}

CORE_NF_BUSINESS_PROFILES = {
    "nrf": {"signaling_load": 0.050, "session_load": 0.018, "user_plane_load": 0.004, "mobility_load": 0.012, "policy_load": 0.018, "auth_load": 0.010},
    "scp": {"signaling_load": 0.060, "session_load": 0.030, "user_plane_load": 0.006, "mobility_load": 0.014, "policy_load": 0.022, "auth_load": 0.012},
    "sepp": {"signaling_load": 0.044, "session_load": 0.020, "user_plane_load": 0.006, "mobility_load": 0.010, "policy_load": 0.025, "auth_load": 0.026},
    "amf": {"signaling_load": 0.072, "session_load": 0.036, "user_plane_load": 0.008, "mobility_load": 0.064, "policy_load": 0.020, "auth_load": 0.034},
    "smf": {"signaling_load": 0.048, "session_load": 0.070, "user_plane_load": 0.018, "mobility_load": 0.025, "policy_load": 0.050, "auth_load": 0.014},
    "upf": {"signaling_load": 0.018, "session_load": 0.055, "user_plane_load": 0.105, "mobility_load": 0.018, "policy_load": 0.018, "auth_load": 0.006},
    "ausf": {"signaling_load": 0.030, "session_load": 0.012, "user_plane_load": 0.002, "mobility_load": 0.014, "policy_load": 0.010, "auth_load": 0.068},
    "udm": {"signaling_load": 0.030, "session_load": 0.044, "user_plane_load": 0.004, "mobility_load": 0.026, "policy_load": 0.035, "auth_load": 0.052},
    "udr": {"signaling_load": 0.022, "session_load": 0.040, "user_plane_load": 0.004, "mobility_load": 0.018, "policy_load": 0.038, "auth_load": 0.048},
    "pcf": {"signaling_load": 0.026, "session_load": 0.045, "user_plane_load": 0.005, "mobility_load": 0.018, "policy_load": 0.075, "auth_load": 0.016},
    "nssf": {"signaling_load": 0.024, "session_load": 0.022, "user_plane_load": 0.004, "mobility_load": 0.035, "policy_load": 0.042, "auth_load": 0.010},
    "bsf": {"signaling_load": 0.022, "session_load": 0.042, "user_plane_load": 0.004, "mobility_load": 0.014, "policy_load": 0.060, "auth_load": 0.014},
}

CORE_NF_ROLES = {
    "upf": "user_plane",
    "pcf": "policy",
    "bsf": "policy",
    "ausf": "auth",
    "udm": "data",
    "udr": "data",
    "nssf": "slice",
    "sepp": "edge",
    "nrf": "registry",
    "scp": "service_proxy",
    "amf": "access_mobility",
    "smf": "session",
}

CORE_NF_DEPENDENCIES = [
    {"source": "nrf", "target": "scp", "criticality": 0.88, "bandwidth_scale": 0.55, "latency_weight": 0.80, "reliability_weight": 0.82},
    {"source": "scp", "target": "amf", "criticality": 1.00, "bandwidth_scale": 0.72, "latency_weight": 1.00, "reliability_weight": 1.00},
    {"source": "scp", "target": "smf", "criticality": 1.00, "bandwidth_scale": 0.76, "latency_weight": 1.00, "reliability_weight": 1.00},
    {"source": "scp", "target": "ausf", "criticality": 0.78, "bandwidth_scale": 0.42, "latency_weight": 0.72, "reliability_weight": 0.78},
    {"source": "scp", "target": "udm", "criticality": 0.82, "bandwidth_scale": 0.44, "latency_weight": 0.76, "reliability_weight": 0.82},
    {"source": "scp", "target": "pcf", "criticality": 0.62, "bandwidth_scale": 0.34, "latency_weight": 0.58, "reliability_weight": 0.62},
    {"source": "scp", "target": "nssf", "criticality": 0.58, "bandwidth_scale": 0.28, "latency_weight": 0.54, "reliability_weight": 0.58},
    {"source": "scp", "target": "bsf", "criticality": 0.48, "bandwidth_scale": 0.24, "latency_weight": 0.48, "reliability_weight": 0.48},
    {"source": "scp", "target": "sepp", "criticality": 0.54, "bandwidth_scale": 0.30, "latency_weight": 0.54, "reliability_weight": 0.56},
    {"source": "amf", "target": "ausf", "criticality": 0.92, "bandwidth_scale": 0.46, "latency_weight": 0.92, "reliability_weight": 0.90},
    {"source": "amf", "target": "udm", "criticality": 0.92, "bandwidth_scale": 0.48, "latency_weight": 0.90, "reliability_weight": 0.92},
    {"source": "amf", "target": "smf", "criticality": 1.00, "bandwidth_scale": 0.86, "latency_weight": 1.00, "reliability_weight": 1.00},
    {"source": "amf", "target": "nssf", "criticality": 0.66, "bandwidth_scale": 0.34, "latency_weight": 0.64, "reliability_weight": 0.66},
    {"source": "smf", "target": "upf", "criticality": 1.00, "bandwidth_scale": 1.00, "latency_weight": 1.00, "reliability_weight": 1.00},
    {"source": "smf", "target": "pcf", "criticality": 0.82, "bandwidth_scale": 0.44, "latency_weight": 0.78, "reliability_weight": 0.80},
    {"source": "smf", "target": "bsf", "criticality": 0.58, "bandwidth_scale": 0.30, "latency_weight": 0.54, "reliability_weight": 0.58},
    {"source": "smf", "target": "udm", "criticality": 0.72, "bandwidth_scale": 0.38, "latency_weight": 0.70, "reliability_weight": 0.72},
    {"source": "udm", "target": "udr", "criticality": 0.86, "bandwidth_scale": 0.56, "latency_weight": 0.78, "reliability_weight": 0.86},
    {"source": "pcf", "target": "udr", "criticality": 0.64, "bandwidth_scale": 0.34, "latency_weight": 0.56, "reliability_weight": 0.64},
    {"source": "pcf", "target": "bsf", "criticality": 0.52, "bandwidth_scale": 0.28, "latency_weight": 0.50, "reliability_weight": 0.52},
]


def zero_business_load() -> Dict[str, float]:
    return {name: 0.0 for name in BUSINESS_DIMENSIONS}


def normalize_nf_type(raw: str) -> str:
    return str(raw or "").lower().replace("-", "_").replace(" ", "_")


def nf_profile(nf_type: str) -> Dict[str, float]:
    return dict(CORE_NF_BUSINESS_PROFILES.get(normalize_nf_type(nf_type), CORE_NF_BUSINESS_PROFILES["amf"]))


def nf_role(nf_type: str) -> str:
    return CORE_NF_ROLES.get(normalize_nf_type(nf_type), "control_plane")


def dependency_key(dep: Dict) -> str:
    return f"{normalize_nf_type(dep.get('source'))}->{normalize_nf_type(dep.get('target'))}"


def dependencies_for_request() -> List[Dict]:
    return [dict(dep) for dep in CORE_NF_DEPENDENCIES]
