"""自动超参报告：基于训练历史给出下一轮建议参数。"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


@dataclass
class MetricsSummary:
    best_success: float
    best_full_sla: float
    last_success: float
    last_sla: float
    last_full_sla: float
    last_alg_latency: float
    last_p95_alg_latency: float
    last_failures: List[Tuple[str, int]]
    success_trend: float
    full_sla_trend: float


def _mean(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def _linear_trend(values: List[float]) -> float:
    """简易线性趋势（每轮变化量），避免依赖额外库。"""
    n = len(values)
    if n < 2:
        return 0.0
    x = list(range(n))
    x_mean = _mean(x)
    y_mean = _mean(values)
    cov = sum((x[i] - x_mean) * (values[i] - y_mean) for i in range(n))
    var = sum((x[i] - x_mean) ** 2 for i in range(n))
    return cov / var if var > 0 else 0.0


def summarize_history(history: List[Dict]) -> MetricsSummary:
    if not history:
        return MetricsSummary(0, 0, 0, 0, 0, 0, 0, [], 0, 0)

    success = [float(h.get("success_rate", 0.0)) for h in history]
    full_sla = [float(h.get("full_sla_satisfaction_rate", 0.0)) for h in history]
    last = history[-1]
    return MetricsSummary(
        best_success=max(success),
        best_full_sla=max(full_sla),
        last_success=float(last.get("success_rate", 0.0)),
        last_sla=float(last.get("sla_satisfaction_rate", 0.0)),
        last_full_sla=float(last.get("full_sla_satisfaction_rate", 0.0)),
        last_alg_latency=float(last.get("avg_algorithm_latency_ms", 0.0)),
        last_p95_alg_latency=float(last.get("p95_algorithm_latency_ms", 0.0)),
        last_failures=list(last.get("top_failure_reasons", [])),
        success_trend=_linear_trend(success[-12:]),
        full_sla_trend=_linear_trend(full_sla[-12:]),
    )


def recommend_next_config(summary: MetricsSummary, current: Dict) -> Dict:
    rec = dict(current)

    # 先给一个轻微增训建议
    rec["epochs"] = int(max(current.get("epochs", 80), current.get("epochs", 80) + 10))

    fail_map = {str(k): int(v) for k, v in summary.last_failures}
    max_steps_fail = fail_map.get("max_steps_reached", 0)
    no_candidates_fail = fail_map.get("no_candidates", 0)
    final_path_down = fail_map.get("final_path_link_down", 0)

    # 成功率偏低：延后严格可靠性、增加探索/样本覆盖
    if summary.last_success < 35.0 or summary.success_trend < -0.8:
        rec["rel_curr_end_epoch"] = int(current.get("rel_curr_end_epoch", 60) + 8)
        rec["rel_curr_strict_ratio"] = round(min(0.9, current.get("rel_curr_strict_ratio", 0.7) + 0.03), 2)
        rec["rel_curr_strict_ramp_ratio"] = round(min(0.4, current.get("rel_curr_strict_ramp_ratio", 0.2) + 0.05), 2)
        rec["rel_curr_min_scale"] = round(max(0.65, current.get("rel_curr_min_scale", 0.75) - 0.02), 2)
        rec["max_data_files"] = int(min(20, current.get("max_data_files", 12) + 2))

    # Full SLA偏低：继续放缓可靠性收紧，提升后期可学习性
    if summary.last_full_sla < 20.0 or summary.full_sla_trend < -0.4:
        rec["rel_curr_end_epoch"] = int(max(rec.get("rel_curr_end_epoch", 60), current.get("rel_curr_end_epoch", 60) + 10))
        rec["rel_curr_strict_ratio"] = round(min(0.92, rec.get("rel_curr_strict_ratio", 0.7) + 0.03), 2)
        rec["rel_curr_min_scale"] = round(max(0.65, rec.get("rel_curr_min_scale", 0.75) - 0.01), 2)

    # 计算长尾高：降低单轮负载并减少共享资源压力
    if summary.last_p95_alg_latency > 1200:
        rec["max_requests_per_file"] = int(max(2, current.get("max_requests_per_file", 3) - 1))
        rec["shared_resources_prob_max"] = round(max(0.35, current.get("shared_resources_prob_max", 0.45) - 0.03), 2)

    # 失败模式微调
    if max_steps_fail > 0:
        rec["warmup_epochs"] = int(max(current.get("warmup_epochs", 15), 18))
    if no_candidates_fail > 0 or final_path_down > 0:
        rec["shared_resources_prob_min"] = round(max(0.18, current.get("shared_resources_prob_min", 0.25) - 0.02), 2)

    return rec


def build_next_run_command(config: Dict) -> str:
    order = [
        "epochs",
        "max_data_files",
        "max_requests_per_file",
        "warmup_epochs",
        "rel_curr_start_epoch",
        "rel_curr_end_epoch",
        "rel_curr_min_scale",
        "rel_curr_strict_ratio",
        "rel_curr_strict_ramp_ratio",
        "shared_resources_prob_min",
        "shared_resources_prob_max",
    ]
    parts = ["python -m ground_training.train"]
    for k in order:
        if k in config:
            parts.append(f"--{k} {config[k]}")
    return " ".join(parts)


def generate_hyperparam_report(
    history: List[Dict],
    current_config: Dict,
    markdown_path: Path,
    json_path: Path,
) -> Tuple[Path, Path]:
    summary = summarize_history(history)
    recommended = recommend_next_config(summary, current_config)
    next_cmd = build_next_run_command(recommended)

    report_obj = {
        "summary": summary.__dict__,
        "current_config": current_config,
        "recommended_config": recommended,
        "next_command": next_cmd,
    }
    json_path.write_text(json.dumps(report_obj, indent=2), encoding="utf-8")

    md = []
    md.append("# Hyperparameter Report")
    md.append("")
    md.append("## Current Summary")
    md.append(f"- Best Success: {summary.best_success:.2f}%")
    md.append(f"- Best Full SLA: {summary.best_full_sla:.2f}%")
    md.append(f"- Last Success: {summary.last_success:.2f}%")
    md.append(f"- Last SLA: {summary.last_sla:.2f}%")
    md.append(f"- Last Full SLA: {summary.last_full_sla:.2f}%")
    md.append(f"- Last Avg Algorithm Latency: {summary.last_alg_latency:.2f} ms")
    md.append(f"- Last P95 Algorithm Latency: {summary.last_p95_alg_latency:.2f} ms")
    md.append(f"- Success Trend (last ~12 epochs): {summary.success_trend:.3f} / epoch")
    md.append(f"- Full SLA Trend (last ~12 epochs): {summary.full_sla_trend:.3f} / epoch")
    md.append(f"- Last Top Failures: {summary.last_failures}")
    md.append("")
    md.append("## Recommended Next Config")
    for k, v in recommended.items():
        if current_config.get(k) != v:
            md.append(f"- `{k}`: {current_config.get(k)} -> **{v}**")
    md.append("")
    md.append("## Next Run Command")
    md.append("```bash")
    md.append(next_cmd)
    md.append("```")
    md.append("")

    markdown_path.write_text("\n".join(md), encoding="utf-8")
    return markdown_path, json_path


def main():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--metrics_json", default="logs/training_metrics.json")
    parser.add_argument("--output_md", default="logs/hyperparam_report.md")
    parser.add_argument("--output_json", default="logs/hyperparam_report.json")
    parser.add_argument("--current_config_json", default="")
    args = parser.parse_args()

    metrics_path = Path(args.metrics_json)
    history = json.loads(metrics_path.read_text(encoding="utf-8")) if metrics_path.exists() else []

    current = {}
    if args.current_config_json:
        current = json.loads(Path(args.current_config_json).read_text(encoding="utf-8"))

    md_path, js_path = generate_hyperparam_report(
        history=history,
        current_config=current,
        markdown_path=Path(args.output_md),
        json_path=Path(args.output_json),
    )
    print(f"Generated hyperparameter report: {md_path}")
    print(f"Generated hyperparameter json: {js_path}")


if __name__ == "__main__":
    main()

