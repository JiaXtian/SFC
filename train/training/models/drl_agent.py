"""DRL Agent（A2C，open5gs核心网部署输入特征）"""
from typing import List, Tuple

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.distributions import Categorical


class ActorNetwork(nn.Module):
    def __init__(self, node_dim=192, vnf_dim=24, context_dim=32, hidden_dim=384):
        super().__init__()
        # Candidate order is produced by the heuristic pruner and carries
        # dynamic resource/path information that is not present in the static
        # GNN embedding.  Encoding rank keeps the ONNX input signature stable
        # while making the hybrid "heuristic pruning + DRL" policy learnable.
        self.rank_feature_dim = 4
        combined_dim = node_dim + vnf_dim + context_dim + self.rank_feature_dim
        self.fc = nn.Sequential(
            nn.Linear(combined_dim, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.GELU(),
            nn.Dropout(0.15),
            nn.Linear(hidden_dim, hidden_dim // 2),
            nn.LayerNorm(hidden_dim // 2),
            nn.GELU(),
            nn.Dropout(0.1),
            nn.Linear(hidden_dim // 2, 1),
        )

    def forward(self, node_emb, candidate_indices, vnf_features, context_features):
        candidate_embs = node_emb[candidate_indices]
        m = candidate_embs.size(0)

        vnf_expanded = vnf_features.unsqueeze(0).expand(m, -1)
        context_expanded = context_features.unsqueeze(0).expand(m, -1)
        rank = torch.arange(m, device=node_emb.device, dtype=candidate_embs.dtype)
        denom = torch.clamp(torch.tensor(float(max(m - 1, 1)), device=node_emb.device, dtype=candidate_embs.dtype), min=1.0)
        rank_norm = rank / denom
        rank_inv = 1.0 - rank_norm
        rank_decay = 1.0 / (rank + 1.0)
        top_band = (rank < 8.0).to(candidate_embs.dtype)
        rank_features = torch.stack([rank_norm, rank_inv, rank_decay, top_band], dim=1)
        combined = torch.cat([candidate_embs, vnf_expanded, context_expanded, rank_features], dim=1)

        logits = self.fc(combined).squeeze(-1)
        probs = F.softmax(logits, dim=0)
        log_probs = F.log_softmax(logits, dim=0)
        return probs, log_probs


class CriticNetwork(nn.Module):
    def __init__(self, input_dim=248, hidden_dim=384):
        super().__init__()
        self.fc = nn.Sequential(
            nn.Linear(input_dim, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.GELU(),
            nn.Dropout(0.15),
            nn.Linear(hidden_dim, hidden_dim // 2),
            nn.LayerNorm(hidden_dim // 2),
            nn.GELU(),
            nn.Dropout(0.1),
            nn.Linear(hidden_dim // 2, 1),
        )

    def forward(self, state_features):
        return self.fc(state_features).squeeze(-1)


class DRLAgent:
    def __init__(
        self,
        node_dim=192,
        vnf_dim=24,
        context_dim=32,
        actor_lr=1e-4,
        critic_lr=3e-4,
        gamma=0.99,
        entropy_coef=0.01,
        imitation_coef=0.20,
        value_loss_coef=0.5,
        max_grad_norm=0.5,
        device="cpu",
    ):
        self.device = device
        self.gamma = gamma
        self.entropy_coef = entropy_coef
        self.imitation_coef = imitation_coef
        self.value_loss_coef = value_loss_coef
        self.max_grad_norm = max_grad_norm
        self.node_dim = node_dim

        self.actor = ActorNetwork(node_dim, vnf_dim, context_dim).to(device)
        self.critic = CriticNetwork(node_dim + vnf_dim + context_dim).to(device)

        self.actor_optimizer = torch.optim.AdamW(self.actor.parameters(), lr=actor_lr, weight_decay=1e-5)
        self.critic_optimizer = torch.optim.AdamW(self.critic.parameters(), lr=critic_lr, weight_decay=1e-5)

        self.actor_losses = []
        self.critic_losses = []
        self.entropies = []

    def select_action(
        self,
        node_embeddings: torch.Tensor,
        candidate_indices: List[int],
        vnf_features: torch.Tensor,
        context_features: torch.Tensor,
        deterministic: bool = False,
    ) -> Tuple[int, float]:
        self.actor.eval()
        with torch.no_grad():
            candidate_tensor = torch.tensor(candidate_indices, dtype=torch.long, device=self.device)
            probs, log_probs = self.actor(node_embeddings, candidate_tensor, vnf_features, context_features)

            if deterministic:
                action = torch.argmax(probs).item()
            else:
                action = Categorical(probs).sample().item()

            log_prob = log_probs[action].item()
        return action, log_prob

    def update(self, trajectories: List[dict]) -> Tuple[float, float, float]:
        self.actor.train()
        self.critic.train()

        if not trajectories:
            return 0.0, 0.0, 0.0

        states = []
        actions = []
        rewards = []
        next_states = []
        dones = []

        for traj in trajectories:
            node_emb = traj["node_embeddings"]
            candidates = torch.tensor(traj["candidate_indices"], dtype=torch.long, device=self.device)
            vnf_feat = traj["vnf_features"]
            ctx_feat = traj["context_features"]

            if len(candidates) > 0:
                avg_candidate_emb = node_emb[candidates].mean(dim=0)
            else:
                avg_candidate_emb = torch.zeros(self.node_dim, device=self.device)

            # Critic分支不反传到GNN，避免与Actor分支共享图导致二次反传错误。
            state_feat = torch.cat([avg_candidate_emb.detach(), vnf_feat.detach(), ctx_feat.detach()])
            states.append(state_feat)
            actions.append(traj["action"])
            rewards.append(traj["reward"])
            dones.append(traj["done"])

            if not traj["done"] and "next_vnf_features" in traj and "next_context_features" in traj:
                next_node_emb = traj.get("next_node_embeddings", node_emb)
                next_candidates = traj.get("next_candidate_indices", traj["candidate_indices"])
                if len(next_candidates) > 0:
                    next_candidate_tensor = torch.tensor(next_candidates, dtype=torch.long, device=self.device)
                    avg_next_emb = next_node_emb[next_candidate_tensor].mean(dim=0)
                else:
                    avg_next_emb = torch.zeros(self.node_dim, device=self.device)
                next_state_feat = torch.cat(
                    [avg_next_emb.detach(), traj["next_vnf_features"].detach(), traj["next_context_features"].detach()]
                )
            else:
                next_state_feat = torch.zeros_like(state_feat)
            next_states.append(next_state_feat)

        states_tensor = torch.stack(states)
        actions_tensor = torch.tensor(actions, dtype=torch.long, device=self.device)
        rewards_tensor = torch.tensor(rewards, dtype=torch.float32, device=self.device)
        next_states_tensor = torch.stack(next_states)
        dones_tensor = torch.tensor(dones, dtype=torch.float32, device=self.device)

        del next_states_tensor
        values = self.critic(states_tensor)
        returns = []
        running_return = torch.tensor(0.0, dtype=torch.float32, device=self.device)
        for reward, done in zip(reversed(rewards_tensor), reversed(dones_tensor)):
            running_return = reward + self.gamma * running_return * (1.0 - done)
            returns.append(running_return)
        returns.reverse()
        targets = torch.stack(returns).detach()

        advantages = targets - values
        if advantages.numel() > 1:
            actor_advantages = (advantages - advantages.mean()) / (advantages.std(unbiased=False) + 1e-6)
        else:
            actor_advantages = advantages
        critic_loss = F.mse_loss(values, targets)

        self.critic_optimizer.zero_grad()
        critic_loss.backward()
        nn.utils.clip_grad_norm_(self.critic.parameters(), self.max_grad_norm)
        self.critic_optimizer.step()

        actor_loss_total = 0.0
        entropy_total = 0.0
        valid_count = 0

        for i, traj in enumerate(trajectories):
            candidates = torch.tensor(traj["candidate_indices"], dtype=torch.long, device=self.device)
            if len(candidates) == 0:
                continue

            probs, log_probs = self.actor(
                traj["node_embeddings"],
                candidates,
                traj["vnf_features"],
                traj["context_features"],
            )

            action = traj["action"]
            if action < 0 or action >= len(candidates):
                continue

            advantage = actor_advantages[i].detach()
            actor_loss = -log_probs[action] * advantage
            imitation_loss = -log_probs[action]

            entropy = Categorical(probs).entropy()
            actor_loss_total += actor_loss + self.imitation_coef * imitation_loss - self.entropy_coef * entropy
            entropy_total += entropy.item()
            valid_count += 1

        if valid_count > 0:
            actor_loss_avg = actor_loss_total / valid_count
            entropy_avg = entropy_total / valid_count

            self.actor_optimizer.zero_grad()
            actor_loss_avg.backward()
            nn.utils.clip_grad_norm_(self.actor.parameters(), self.max_grad_norm)
            self.actor_optimizer.step()
        else:
            actor_loss_avg = torch.tensor(0.0, device=self.device)
            entropy_avg = 0.0

        self.actor_losses.append(actor_loss_avg.item())
        self.critic_losses.append(critic_loss.item())
        self.entropies.append(entropy_avg)

        return actor_loss_avg.item(), critic_loss.item(), entropy_avg

    def save(self, path):
        torch.save(
            {
                "actor_state_dict": self.actor.state_dict(),
                "critic_state_dict": self.critic.state_dict(),
                "actor_optimizer": self.actor_optimizer.state_dict(),
                "critic_optimizer": self.critic_optimizer.state_dict(),
            },
            path,
        )

    def load(self, path, load_optimizer: bool = True):
        """加载模型权重。

        Args:
            path: checkpoint 文件路径。
            load_optimizer: 是否恢复优化器状态。导出/推理场景设为 False 可避免
                因优化器参数组数量不匹配（例如训练时调用了 add_param_group 添加
                GNN 参数）而引发 ValueError。
        """
        checkpoint = torch.load(path, map_location=self.device, weights_only=False)
        self.actor.load_state_dict(checkpoint["actor_state_dict"])
        self.critic.load_state_dict(checkpoint["critic_state_dict"])
        if load_optimizer:
            if "actor_optimizer" in checkpoint:
                try:
                    self.actor_optimizer.load_state_dict(checkpoint["actor_optimizer"])
                except ValueError as e:
                    import warnings
                    warnings.warn(
                        f"actor_optimizer 状态加载失败（参数组数量不匹配），已跳过优化器恢复: {e}"
                    )
            if "critic_optimizer" in checkpoint:
                try:
                    self.critic_optimizer.load_state_dict(checkpoint["critic_optimizer"])
                except ValueError as e:
                    import warnings
                    warnings.warn(
                        f"critic_optimizer 状态加载失败（参数组数量不匹配），已跳过优化器恢复: {e}"
                    )
