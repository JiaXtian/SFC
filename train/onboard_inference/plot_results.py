from __future__ import annotations

import argparse
import json
import math
import os
from collections import defaultdict
from pathlib import Path

os.environ.setdefault("MPLBACKEND", "Agg")
os.environ.setdefault("MPLCONFIGDIR", str(Path.cwd() / "logs" / ".mplconfig"))

import matplotlib.pyplot as plt
import numpy as np


def load_results(path: Path) -> dict:
    with path.open() as f:
        return json.load(f)


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def confidence_interval_95(values: list[float]) -> tuple[float, float]:
    if not values:
        return 0.0, 0.0
    arr = np.asarray(values, dtype=float)
    mean = float(np.mean(arr))
    if arr.size == 1:
        return mean, 0.0
    sem = float(np.std(arr, ddof=1) / math.sqrt(arr.size))
    return mean, 1.96 * sem


def build_topology_rows(results: dict) -> list[dict]:
    rows: list[dict] = []
    for topo in results.get("topologies", []):
        rows.append(
            {
                "topology_name": Path(topo.get("topology_file", "unknown")).stem,
                "scale": int(topo.get("topology_scale", 0)),
                "scale_bucket": str(topo.get("scale_bucket", "unknown")),
                "success_rate": float(topo.get("success_rate", 0.0)) * 100.0,
                "avg_time_per_sfc_ms": float(topo.get("total_time_ms", 0.0))
                / max(1, int(topo.get("total_requests", 1))),
                "avg_success_delay_ms": float(topo.get("average_delay_ms", 0.0)),
                "total_requests": int(topo.get("total_requests", 0)),
            }
        )
    return rows


def build_sfc_delay_rows(results: dict) -> dict[str, list[float]]:
    by_bucket: dict[str, list[float]] = defaultdict(list)
    for topo in results.get("topologies", []):
        bucket = str(topo.get("scale_bucket", "unknown"))
        for item in topo.get("sfc_results", []):
            if bool(item.get("success", False)):
                by_bucket[bucket].append(float(item.get("total_delay_ms", 0.0)))
    return by_bucket


def plot_speed_vs_scale(rows: list[dict], out_dir: Path) -> Path:
    scales = np.asarray([max(1, int(r["scale"])) for r in rows], dtype=float)
    speeds = np.asarray([float(r["avg_time_per_sfc_ms"]) for r in rows], dtype=float)
    success_rates = np.asarray([float(r["success_rate"]) for r in rows], dtype=float)

    fig, ax = plt.subplots(figsize=(8.8, 5.8))
    scatter = ax.scatter(
        scales,
        speeds,
        c=success_rates,
        cmap="viridis",
        s=72,
        alpha=0.88,
        edgecolors="black",
        linewidths=0.35,
    )
    cbar = fig.colorbar(scatter, ax=ax)
    cbar.set_label("Success Rate (%)")

    log_scales = np.log10(scales)
    if len(scales) >= 2:
        coef = np.polyfit(log_scales, speeds, deg=1)
        pred = np.polyval(coef, log_scales)
        ss_res = float(np.sum((speeds - pred) ** 2))
        ss_tot = float(np.sum((speeds - np.mean(speeds)) ** 2))
        r2 = 1.0 - ss_res / ss_tot if ss_tot > 1e-12 else 0.0

        x_grid = np.linspace(np.min(scales), np.max(scales), 160)
        y_grid = np.polyval(coef, np.log10(x_grid))
        ax.plot(x_grid, y_grid, color="#d62728", linewidth=1.8, label="Log-Linear Fit")
        ax.text(
            0.02,
            0.98,
            f"t(ms) = {coef[0]:.2f}·log10(N) + {coef[1]:.2f}\n$R^2$ = {r2:.3f}",
            transform=ax.transAxes,
            va="top",
            ha="left",
            fontsize=10,
            bbox=dict(facecolor="white", edgecolor="#777", alpha=0.88),
        )

    ax.set_xscale("log")
    ax.set_xlabel("Constellation Scale N (satellites, log10)")
    ax.set_ylabel("Inference Time Per SFC (ms)")
    ax.set_title("Inference Speed Scaling Across Constellation Topologies")
    ax.grid(True, which="both", linestyle="--", alpha=0.22)
    if len(scales) >= 2:
        ax.legend(loc="lower right", frameon=True)

    fig.tight_layout()
    path = out_dir / "speed_vs_scale.png"
    fig.savefig(path, dpi=220)
    plt.close(fig)
    return path


def plot_bucket_speed_with_ci(rows: list[dict], out_dir: Path) -> Path:
    grouped: dict[str, list[float]] = defaultdict(list)
    for r in rows:
        grouped[r["scale_bucket"]].append(float(r["avg_time_per_sfc_ms"]))

    labels = sorted(grouped.keys())
    means = []
    cis = []
    for label in labels:
        mean, ci = confidence_interval_95(grouped[label])
        means.append(mean)
        cis.append(ci)

    x = np.arange(len(labels))
    fig, ax = plt.subplots(figsize=(8.6, 5.5))
    bars = ax.bar(
        x,
        means,
        yerr=cis,
        capsize=6,
        color="#4c78a8",
        edgecolor="black",
        linewidth=0.45,
        alpha=0.9,
    )
    for idx, b in enumerate(bars):
        ax.text(
            b.get_x() + b.get_width() / 2,
            b.get_height() + max(cis[idx], 0.1) + 0.35,
            f"{means[idx]:.2f}",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=15, ha="right")
    ax.set_ylabel("Avg Inference Time Per SFC (ms)")
    ax.set_title("Inference Speed by Scale Bucket (Mean ± 95% CI)")
    ax.grid(axis="y", linestyle="--", alpha=0.24)
    fig.tight_layout()

    path = out_dir / "speed_bucket_ci.png"
    fig.savefig(path, dpi=220)
    plt.close(fig)
    return path


def plot_success_latency_tradeoff(rows: list[dict], out_dir: Path) -> Path:
    success = np.asarray([float(r["success_rate"]) for r in rows], dtype=float)
    speed = np.asarray([float(r["avg_time_per_sfc_ms"]) for r in rows], dtype=float)
    size = np.asarray([max(40, min(260, int(r["scale"]) // 20)) for r in rows], dtype=float)

    fig, ax = plt.subplots(figsize=(8.8, 5.8))
    ax.scatter(
        speed,
        success,
        s=size,
        c="#59a14f",
        alpha=0.76,
        edgecolors="black",
        linewidths=0.3,
    )
    ax.set_xlabel("Inference Time Per SFC (ms)")
    ax.set_ylabel("Success Rate (%)")
    ax.set_title("Strategy Quality vs Inference Speed Trade-off")
    ax.grid(True, linestyle="--", alpha=0.24)
    fig.tight_layout()

    path = out_dir / "success_speed_tradeoff.png"
    fig.savefig(path, dpi=220)
    plt.close(fig)
    return path


def plot_delay_box_by_bucket(delay_by_bucket: dict[str, list[float]], out_dir: Path) -> Path:
    labels = sorted(delay_by_bucket.keys())
    data = [delay_by_bucket[label] for label in labels]

    fig, ax = plt.subplots(figsize=(8.8, 5.8))
    box = ax.boxplot(
        data,
        labels=labels,
        patch_artist=True,
        showmeans=True,
        meanline=True,
        medianprops={"color": "#c44e52", "linewidth": 1.3},
        meanprops={"color": "#2f4b7c", "linewidth": 1.3},
    )
    palette = ["#4e79a7", "#59a14f", "#e15759", "#f28e2b", "#76b7b2", "#edc948"]
    for i, patch in enumerate(box["boxes"]):
        patch.set_facecolor(palette[i % len(palette)])
        patch.set_alpha(0.55)

    ax.set_title("Successful Deployment Delay Distribution by Scale Bucket")
    ax.set_ylabel("End-to-End Deployment Delay (ms)")
    ax.grid(axis="y", linestyle="--", alpha=0.23)
    ax.tick_params(axis="x", rotation=12)
    fig.tight_layout()

    path = out_dir / "delay_box_bucket.png"
    fig.savefig(path, dpi=220)
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

    plt.rcParams.update(
        {
            "font.size": 10,
            "axes.titlesize": 12,
            "axes.labelsize": 10,
            "legend.fontsize": 9,
            "figure.facecolor": "white",
            "axes.facecolor": "white",
        }
    )

    results = load_results(input_path)
    rows = build_topology_rows(results)
    delay_by_bucket = build_sfc_delay_rows(results)

    if not rows:
        raise RuntimeError("No topology rows found in result JSON; cannot plot.")

    generated = [
        plot_speed_vs_scale(rows, output_dir),
        plot_bucket_speed_with_ci(rows, output_dir),
        plot_success_latency_tradeoff(rows, output_dir),
    ]
    if any(delay_by_bucket.values()):
        generated.append(plot_delay_box_by_bucket(delay_by_bucket, output_dir))

    print("Research-style inference plots generated:")
    for path in generated:
        print(f"  - {path}")


if __name__ == "__main__":
    main()
