"""ONNX模型导出（扩展输入维度）"""
import os
import sys

import torch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ground_training.models.drl_agent import DRLAgent
from ground_training.models.gnn_encoder import GNNEncoder


def export():
    print("=" * 60)
    print("  导出ONNX模型 ")
    print("=" * 60)
    os.makedirs("../../models/exported", exist_ok=True)

    gnn_path = "../../models/checkpoints/gnn_best.pth" if os.path.exists("../../models/checkpoints/gnn_best.pth") else "../../models/checkpoints/gnn_final.pth"
    agent_path = "../../models/checkpoints/model_best.pth" if os.path.exists("../../models/checkpoints/model_best.pth") else "../../models/checkpoints/model_final.pth"

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
        "models/exported/gnn_encoder.onnx",
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
    print("  ✓ GNN导出完成: models/exported/gnn_encoder.onnx")

    print("\n[2/2] 导出Actor网络...")
    agent = DRLAgent(node_dim=192, vnf_dim=4, context_dim=48, device="cpu")
    agent.load(agent_path, load_optimizer=False)
    agent.actor.eval()

    dummy_emb = torch.randn(120, 192)
    dummy_cand = torch.tensor([0, 1, 2, 3, 4, 5], dtype=torch.long)
    dummy_vnf = torch.randn(4)
    dummy_ctx = torch.randn(48)

    torch.onnx.export(
        agent.actor,
        (dummy_emb, dummy_cand, dummy_vnf, dummy_ctx),
        "models/exported/actor.onnx",
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
    print("   Actor导出完成: models/exported/actor.onnx")

    print("\n" + "=" * 60)
    print("   ONNX导出完成")
    print("=" * 60)
    print("节点输入维度: 8")
    print("VNF输入维度: 4")
    print("上下文输入维度: 48")


if __name__ == "__main__":
    export()
