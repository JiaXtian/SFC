from __future__ import annotations

import argparse
import json
import os
from collections import Counter
from pathlib import Path

os.environ.setdefault("MPLBACKEND", "Agg")
os.environ.setdefault("MPLCONFIGDIR", str(Path.cwd() / "logs" / ".mplconfig"))

import matplotlib.pyplot as plt


def load_results(path: Path) -> dict:
    with path.open() as f:
        return json.load(f)


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def plot_topology_success(topologies: list[dict], out_dir: Path) -> Path:
    labels = [Path(t["topology_file"]).stem for t in topologies]
    success_rates = [100.0 * t.get("success_rate", 0.0) for t in topologies]
    scales = [t.get("topology_scale", 0) for t in topologies]

    fig, ax = plt.subplots(figsize=(max(10, len(labels) * 1.1), 6))
    bars = ax.bar(range(len(labels)), success_rates, color="#2f6b8a")
    ax.set_title("Per-Topology Success Rate")
    ax.set_ylabel("Success Rate (%)")
    ax.set_xlabel("Topology")
    ax.set_ylim(0, 100)
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.grid(axis="y", alpha=0.25)

    for bar, rate, scale in zip(bars, success_rates, scales):
        ax.text(bar.get_x() + bar.get_width() / 2, rate + 1.0, f"{rate:.1f}%\n{scale}", ha="center", va="bottom", fontsize=8)

    fig.tight_layout()
    path = out_dir / "topology_success_rates.png"
    fig.savefig(path, dpi=180)
    plt.close(fig)
    return path


def plot_scale_analysis(scale_analysis: list[dict], out_dir: Path) -> Path:
    labels = [item["scale_bucket"] for item in scale_analysis]
    success_rates = [100.0 * item.get("success_rate", 0.0) for item in scale_analysis]
    avg_time = [item.get("avg_time_per_sfc_ms", 0.0) for item in scale_analysis]

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    axes[0].bar(labels, success_rates, color="#5b8c5a")
    axes[0].set_title("Success Rate by Scale Bucket")
    axes[0].set_ylabel("Success Rate (%)")
    axes[0].set_ylim(0, 100)
    axes[0].grid(axis="y", alpha=0.25)
    axes[0].tick_params(axis="x", rotation=20)

    axes[1].bar(labels, avg_time, color="#c17c3a")
    axes[1].set_title("Avg Inference Time per SFC")
    axes[1].set_ylabel("Time (ms)")
    axes[1].grid(axis="y", alpha=0.25)
    axes[1].tick_params(axis="x", rotation=20)

    fig.tight_layout()
    path = out_dir / "scale_analysis.png"
    fig.savefig(path, dpi=180)
    plt.close(fig)
    return path


def plot_failure_reasons(topologies: list[dict], out_dir: Path) -> Path:
    counter = Counter()
    for topo in topologies:
        counter.update(topo.get("failure_reason_counts", {}))

    top_items = counter.most_common(8)
    labels = [item[0] for item in top_items]
    values = [item[1] for item in top_items]

    fig, ax = plt.subplots(figsize=(12, 6))
    ax.barh(range(len(labels)), values, color="#b54d4d")
    ax.set_title("Top Failure Reasons")
    ax.set_xlabel("Count")
    ax.set_yticks(range(len(labels)))
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.grid(axis="x", alpha=0.25)

    fig.tight_layout()
    path = out_dir / "failure_reasons.png"
    fig.savefig(path, dpi=180)
    plt.close(fig)
    return path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    ensure_dir(output_dir)

    results = load_results(input_path)
    topologies = results.get("topologies", [])
    scale_analysis = results.get("scale_analysis", [])

    generated = [
        plot_topology_success(topologies, output_dir),
        plot_scale_analysis(scale_analysis, output_dir),
        plot_failure_reasons(topologies, output_dir),
    ]

    print("Inference plots generated:")
    for path in generated:
        print(f"  - {path}")


if __name__ == "__main__":
    main()
