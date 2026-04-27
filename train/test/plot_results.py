from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

os.environ.setdefault("MPLBACKEND", "Agg")
os.environ.setdefault("MPLCONFIGDIR", str(Path.cwd() / "logs" / ".mplconfig"))

import matplotlib.pyplot as plt
import numpy as np


def load_results(path: Path) -> dict:
    with path.open() as f:
        return json.load(f)


def build_topology_latency_rows(results: dict) -> list[dict]:
    rows: list[dict] = []
    for topo in results.get("topologies", []):
        rows.append(
            {
                "name": Path(topo.get("topology_file", "unknown")).stem,
                "scale": int(topo.get("topology_scale", 0)),
                "p50_ms": float(topo.get("inference_time_p50_ms", 0.0)),
                "p95_ms": float(topo.get("inference_time_p95_ms", 0.0)),
                "avg_ms": float(topo.get("total_time_ms", 0.0)) / max(1, int(topo.get("total_requests", 1))),
            }
        )
    rows.sort(key=lambda x: x["scale"])
    return rows


def plot_latency_by_topology(rows: list[dict], output_path: Path) -> Path:
    x = np.arange(len(rows), dtype=float)
    avg = np.asarray([r["avg_ms"] for r in rows], dtype=float)
    p50 = np.asarray([r["p50_ms"] for r in rows], dtype=float)
    p95 = np.asarray([r["p95_ms"] for r in rows], dtype=float)
    labels = [f"{r['name']}\nN={r['scale']}" for r in rows]

    fig, ax = plt.subplots(figsize=(13.5, 6.5))
    ax.plot(x, avg, marker="o", linewidth=2.0, label="Avg")
    ax.plot(x, p50, marker="s", linewidth=1.8, label="P50")
    ax.plot(x, p95, marker="^", linewidth=1.8, label="P95")

    ax.set_title("Inference Latency Across Different Topologies")
    ax.set_xlabel("Topology")
    ax.set_ylabel("Latency (ms)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=25, ha="right")
    ax.grid(axis="y", linestyle="--", alpha=0.3)
    ax.legend(loc="upper left")

    fig.tight_layout()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=220)
    plt.close(fig)
    return output_path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    results = load_results(Path(args.input))
    rows = build_topology_latency_rows(results)
    if not rows:
        raise RuntimeError("No topology rows found in result JSON; cannot plot.")

    output_path = plot_latency_by_topology(rows, Path(args.output))
    print(f"Latency plot generated: {output_path}")


if __name__ == "__main__":
    main()
