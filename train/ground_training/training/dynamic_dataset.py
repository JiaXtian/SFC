"""动态时序训练数据加载工具。"""
from __future__ import annotations

import glob
import json
import os
from dataclasses import dataclass
from typing import Dict, List, Sequence, Tuple

from ground_training.training.trainer import SFCTrainer


@dataclass
class DynamicSequenceSample:
    topology_file: str
    request_file: str
    topology_timeline: List[Dict]
    request_timeline: List[Dict]


def _extract_numeric_suffix(path: str) -> str:
    base = os.path.splitext(os.path.basename(path))[0]
    token = base.split("_")[-1]
    return token if token.isdigit() else base


def discover_dynamic_pairs(topology_dir: str, request_dir: str) -> List[Tuple[str, str]]:
    topology_files = sorted(glob.glob(os.path.join(topology_dir, "*.json")))
    request_files = sorted(glob.glob(os.path.join(request_dir, "*.json")))
    if not topology_files or not request_files:
        return []

    req_by_suffix = {_extract_numeric_suffix(p): p for p in request_files}
    pairs = []
    for topo in topology_files:
        suffix = _extract_numeric_suffix(topo)
        req = req_by_suffix.get(suffix)
        if req:
            pairs.append((topo, req))
    if pairs:
        return pairs

    min_len = min(len(topology_files), len(request_files))
    return list(zip(topology_files[:min_len], request_files[:min_len]))


def load_dynamic_sequences(pairs: Sequence[Tuple[str, str]], max_sequences: int = 0) -> List[Dict]:
    samples: List[Dict] = []
    selected_pairs = list(pairs)
    if max_sequences > 0:
        selected_pairs = selected_pairs[:max_sequences]

    for topo_file, req_file in selected_pairs:
        with open(topo_file) as f:
            topo_payload = json.load(f)
        with open(req_file) as f:
            req_payload = json.load(f)

        sample = DynamicSequenceSample(
            topology_file=topo_file,
            request_file=req_file,
            topology_timeline=SFCTrainer.normalize_topology_timeline(topo_payload),
            request_timeline=SFCTrainer.normalize_request_timeline(req_payload),
        )
        if sample.topology_timeline and sample.request_timeline:
            samples.append(sample.__dict__)
    return samples

