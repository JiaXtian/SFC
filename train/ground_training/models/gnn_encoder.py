"""GNN编码器（增强架构，支持8维节点输入）"""
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch_geometric.nn import GCNConv


class GNNEncoder(nn.Module):
    def __init__(self, input_dim=8, hidden_dim=192, num_layers=4, dropout=0.15):
        super().__init__()
        self.input_proj = nn.Linear(input_dim, hidden_dim)
        self.convs = nn.ModuleList([GCNConv(hidden_dim, hidden_dim) for _ in range(num_layers)])
        self.norms = nn.ModuleList([nn.LayerNorm(hidden_dim) for _ in range(num_layers)])
        self.dropout = nn.Dropout(dropout)

    def forward(self, x, edge_index):
        h = self.input_proj(x)
        for conv, norm in zip(self.convs, self.norms):
            residual = h
            h = conv(h, edge_index)
            h = norm(h)
            h = F.gelu(h)
            h = self.dropout(h)
            h = h + residual
        return h
