"""ONNX模型导出（扩展输入维度）"""
import argparse
import os
import sys

import torch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from ground_training.models.drl_agent import DRLAgent
from ground_training.models.gnn_encoder import GNNEncoder


def _resolve_checkpoint(*candidates):
    for path in candidates:
        if path and os.path.exists(path):
            return path
    return candidates[0] if candidates else None


def export_models(gnn_path=None, agent_path=None, output_dir="models/exported", context_dim=48):
    print("=" * 60)
    print("  导出ONNX模型 ")
    print("=" * 60)
    os.makedirs(output_dir, exist_ok=True)

    if gnn_path is None:
        gnn_path = _resolve_checkpoint(
            "models/checkpoints/gnn_best.pth",
            "models/checkpoints/gnn_final.pth",
            "../../models/checkpoints/gnn_best.pth",
            "../../models/checkpoints/gnn_final.pth",
        )
    if agent_path is None:
        agent_path = _resolve_checkpoint(
            "models/checkpoints/model_best.pth",
            "models/checkpoints/model_final.pth",
            "../../models/checkpoints/model_best.pth",
            "../../models/checkpoints/model_final.pth",
        )

    if not os.path.exists(gnn_path) or not os.path.exists(agent_path):
        print("错误: 缺少训练模型，请先运行训练")
        sys.exit(1)

    print("\n[1/2] 导出GNN编码器...")
    gnn = GNNEncoder(input_dim=8, hidden_dim=192, num_layers=4)
    gnn.load_state_dict(torch.load(gnn_path, map_location="cpu"))
    gnn.eval()

    dummy_x = torch.randn(120, 8)
    dummy_edge = torch.randint(0, 120, (2, 320), dtype=torch.long)

    torch.onnx.export(
        gnn,
        (dummy_x, dummy_edge),
        os.path.join(output_dir, "gnn_encoder.onnx"),
        input_names=["node_features", "edge_index"],
        output_names=["node_embeddings"],
        dynamic_axes={
            "node_features": {0: "num_nodes"},
            "edge_index": {1: "num_edges"},
            "node_embeddings": {0: "num_nodes"},
        },
        opset_version=12,
        dynamo=False,
        verbose=False,
    )
    print(f"  ✓ GNN导出完成: {os.path.join(output_dir, 'gnn_encoder.onnx')}")

    print("\n[2/2] 导出Actor网络...")
    agent = DRLAgent(node_dim=192, vnf_dim=4, context_dim=context_dim, device="cpu")
    agent.load(agent_path, load_optimizer=False)
    agent.actor.eval()

    dummy_emb = torch.randn(120, 192)
    dummy_cand = torch.tensor([0, 1, 2, 3, 4, 5], dtype=torch.long)
    dummy_vnf = torch.randn(4)
    dummy_ctx = torch.randn(context_dim)

    torch.onnx.export(
        agent.actor,
        (dummy_emb, dummy_cand, dummy_vnf, dummy_ctx),
        os.path.join(output_dir, "actor.onnx"),
        input_names=["node_embeddings", "candidate_indices", "vnf_features", "context_features"],
        output_names=["probs", "logits"],
        dynamic_axes={
            "node_embeddings": {0: "num_nodes"},
            "candidate_indices": {0: "num_candidates"},
            "probs": {0: "num_candidates"},
            "logits": {0: "num_candidates"},
        },
        opset_version=12,
        dynamo=False,
        verbose=False,
    )
    print(f"   Actor导出完成: {os.path.join(output_dir, 'actor.onnx')}")

    print("\n" + "=" * 60)
    print("   ONNX导出完成")
    print("=" * 60)
    print("节点输入维度: 8")
    print("VNF输入维度: 4")
    print(f"上下文输入维度: {context_dim}")


def export():
    parser = argparse.ArgumentParser(description="Export ONNX models")
    parser.add_argument("--gnn-checkpoint", default=None)
    parser.add_argument("--actor-checkpoint", default=None)
    parser.add_argument("--output-dir", default="models/exported")
    parser.add_argument("--context-dim", type=int, default=48)
    args = parser.parse_args()

    export_models(
        gnn_path=args.gnn_checkpoint,
        agent_path=args.actor_checkpoint,
        output_dir=args.output_dir,
        context_dim=args.context_dim,
    )


if __name__ == "__main__":
    export()
