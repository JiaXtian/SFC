#include "drl_inference.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <numeric>
#include <string>

namespace sfc {

namespace {

int64_t normalize_dim(int64_t actual, int64_t fallback, const char* tag) {
    if (actual > 0) {
        return actual;
    }
    std::cerr << "[DRL Inference] Dimension for " << tag
              << " is dynamic/unknown, fallback to " << fallback << std::endl;
    return fallback;
}

} // namespace

DRLInference::DRLInference() {
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "SFC");
    session_options_.SetIntraOpNumThreads(1);
    session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
}

DRLInference::~DRLInference() = default;

std::vector<float> DRLInference::align_flat_features(
    const std::vector<float>& features,
    size_t target_dim,
    const char* feature_name) const {
    if (target_dim == 0) {
        return {};
    }
    if (features.size() == target_dim) {
        return features;
    }

    std::vector<float> aligned(target_dim, 0.0f);
    const size_t copy_dim = std::min(features.size(), target_dim);
    std::copy_n(features.begin(), copy_dim, aligned.begin());

    std::cerr << "[DRL Inference] Feature adapt for " << feature_name
              << ": input_dim=" << features.size()
              << ", model_dim=" << target_dim
              << " (truncate/pad applied)" << std::endl;
    return aligned;
}

std::vector<float> DRLInference::align_node_features(
    const std::vector<float>& node_features,
    size_t num_nodes,
    size_t target_dim) const {
    if (num_nodes == 0) {
        return {};
    }
    if (node_features.size() % num_nodes != 0) {
        std::cerr << "[DRL Inference] Node feature size is not divisible by num_nodes: "
                  << node_features.size() << " vs " << num_nodes << std::endl;
        return {};
    }

    const size_t input_dim = node_features.size() / num_nodes;
    if (input_dim == target_dim) {
        return node_features;
    }

    std::vector<float> aligned(num_nodes * target_dim, 0.0f);
    const size_t copy_dim = std::min(input_dim, target_dim);
    for (size_t i = 0; i < num_nodes; ++i) {
        const float* src = node_features.data() + i * input_dim;
        float* dst = aligned.data() + i * target_dim;
        std::copy_n(src, copy_dim, dst);
    }

    std::cerr << "[DRL Inference] Node feature adapt: input_dim=" << input_dim
              << ", model_dim=" << target_dim
              << " (truncate/pad applied)" << std::endl;
    return aligned;
}

bool DRLInference::load_models(const std::string& gnn_path, const std::string& actor_path) {
    try {
        gnn_session_ = std::make_unique<Ort::Session>(*env_, gnn_path.c_str(), session_options_);
        std::cout << "[DRL Inference] Loading GNN model: " << gnn_path << std::endl;

        actor_session_ = std::make_unique<Ort::Session>(*env_, actor_path.c_str(), session_options_);
        std::cout << "[DRL Inference] Loading Actor model: " << actor_path << std::endl;

        const auto get_last_dim_from_input = [](Ort::Session& session, size_t input_idx) -> int64_t {
            auto info = session.GetInputTypeInfo(input_idx).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (shape.empty()) {
                return -1;
            }
            return shape.back();
        };
        const auto get_last_dim_from_output = [](Ort::Session& session, size_t output_idx) -> int64_t {
            auto info = session.GetOutputTypeInfo(output_idx).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (shape.empty()) {
                return -1;
            }
            return shape.back();
        };

        if (gnn_session_->GetInputCount() < 1 || gnn_session_->GetOutputCount() < 1 || actor_session_->GetInputCount() < 4) {
            std::cerr << "[DRL Inference] Invalid model input signatures." << std::endl;
            return false;
        }

        const int64_t gnn_node_dim = get_last_dim_from_input(*gnn_session_, 0);
        const int64_t gnn_output_dim = get_last_dim_from_output(*gnn_session_, 0);
        const int64_t actor_emb_dim = get_last_dim_from_input(*actor_session_, 0);
        const int64_t actor_vnf_dim = get_last_dim_from_input(*actor_session_, 2);
        const int64_t actor_ctx_dim = get_last_dim_from_input(*actor_session_, 3);

        node_feature_dim_ = normalize_dim(gnn_node_dim, kDefaultNodeFeatureDim, "gnn.node_features");
        embedding_dim_ = normalize_dim(actor_emb_dim, kDefaultEmbeddingDim, "actor.node_embeddings");
        vnf_feature_dim_ = normalize_dim(actor_vnf_dim, kDefaultVnfFeatureDim, "actor.vnf_features");
        context_feature_dim_ = normalize_dim(actor_ctx_dim, kDefaultContextFeatureDim, "actor.context_features");

        if (gnn_output_dim > 0 && gnn_output_dim != embedding_dim_) {
            std::cerr << "[DRL Inference] GNN output dim (" << gnn_output_dim
                      << ") != Actor embedding dim (" << embedding_dim_
                      << "), runtime will truncate/pad node embeddings." << std::endl;
        }

        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const auto run_gnn_probe = [&](int64_t node_dim) -> bool {
            try {
                std::vector<float> probe_nodes(2 * static_cast<size_t>(node_dim), 0.0f);
                std::vector<int64_t> probe_edges = {0, 1, 1, 0};
                std::vector<int64_t> node_shape = {2, node_dim};
                std::vector<int64_t> edge_shape = {2, 2};
                std::vector<Ort::Value> probe_inputs;
                probe_inputs.push_back(Ort::Value::CreateTensor<float>(
                    memory_info,
                    probe_nodes.data(),
                    probe_nodes.size(),
                    node_shape.data(),
                    node_shape.size()));
                probe_inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                    memory_info,
                    probe_edges.data(),
                    probe_edges.size(),
                    edge_shape.data(),
                    edge_shape.size()));
                const char* gnn_input_names[] = {"node_features", "edge_index"};
                const char* gnn_output_names[] = {"node_embeddings"};
                gnn_session_->Run(
                    Ort::RunOptions{nullptr},
                    gnn_input_names,
                    probe_inputs.data(),
                    2,
                    gnn_output_names,
                    1);
                return true;
            } catch (const Ort::Exception&) {
                return false;
            }
        };
        const auto run_actor_probe = [&](int64_t emb_dim, int64_t vnf_dim, int64_t ctx_dim) -> bool {
            try {
                std::vector<float> probe_emb(2 * static_cast<size_t>(emb_dim), 0.0f);
                std::vector<int64_t> probe_candidates = {0, 1};
                std::vector<float> probe_vnf(static_cast<size_t>(vnf_dim), 0.0f);
                std::vector<float> probe_ctx(static_cast<size_t>(ctx_dim), 0.0f);
                std::vector<int64_t> emb_shape = {2, emb_dim};
                std::vector<int64_t> cand_shape = {2};
                std::vector<int64_t> vnf_shape = {vnf_dim};
                std::vector<int64_t> ctx_shape = {ctx_dim};
                std::vector<Ort::Value> probe_inputs;
                probe_inputs.push_back(Ort::Value::CreateTensor<float>(
                    memory_info,
                    probe_emb.data(),
                    probe_emb.size(),
                    emb_shape.data(),
                    emb_shape.size()));
                probe_inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                    memory_info,
                    probe_candidates.data(),
                    probe_candidates.size(),
                    cand_shape.data(),
                    cand_shape.size()));
                probe_inputs.push_back(Ort::Value::CreateTensor<float>(
                    memory_info,
                    probe_vnf.data(),
                    probe_vnf.size(),
                    vnf_shape.data(),
                    vnf_shape.size()));
                probe_inputs.push_back(Ort::Value::CreateTensor<float>(
                    memory_info,
                    probe_ctx.data(),
                    probe_ctx.size(),
                    ctx_shape.data(),
                    ctx_shape.size()));
                const char* actor_input_names[] = {"node_embeddings", "candidate_indices", "vnf_features", "context_features"};
                const char* actor_output_names[] = {"probs", "logits"};
                actor_session_->Run(
                    Ort::RunOptions{nullptr},
                    actor_input_names,
                    probe_inputs.data(),
                    4,
                    actor_output_names,
                    2);
                return true;
            } catch (const Ort::Exception&) {
                return false;
            }
        };
        const auto push_unique = [](std::vector<int64_t>& values, int64_t value) {
            if (value <= 0) {
                return;
            }
            if (std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(value);
            }
        };

        bool gnn_ok = run_gnn_probe(node_feature_dim_);
        if (!gnn_ok) {
            std::vector<int64_t> node_candidates;
            push_unique(node_candidates, node_feature_dim_);
            push_unique(node_candidates, 18);
            push_unique(node_candidates, 8);
            push_unique(node_candidates, 14);
            for (int64_t candidate : node_candidates) {
                if (candidate == node_feature_dim_) {
                    continue;
                }
                if (run_gnn_probe(candidate)) {
                    node_feature_dim_ = candidate;
                    gnn_ok = true;
                    break;
                }
            }
        }
        if (!gnn_ok) {
            std::cerr << "[DRL Inference] Failed to probe compatible GNN input dimension." << std::endl;
            return false;
        }

        bool actor_ok = run_actor_probe(embedding_dim_, vnf_feature_dim_, context_feature_dim_);
        if (!actor_ok) {
            std::vector<int64_t> emb_candidates;
            std::vector<int64_t> vnf_candidates;
            std::vector<int64_t> ctx_candidates;
            push_unique(emb_candidates, embedding_dim_);
            push_unique(emb_candidates, 192);
            push_unique(vnf_candidates, vnf_feature_dim_);
            push_unique(vnf_candidates, 24);
            push_unique(vnf_candidates, 8);
            push_unique(vnf_candidates, 4);
            push_unique(ctx_candidates, context_feature_dim_);
            push_unique(ctx_candidates, 32);
            push_unique(ctx_candidates, 48);
            for (int64_t emb_dim : emb_candidates) {
                bool found = false;
                for (int64_t vnf_dim : vnf_candidates) {
                    for (int64_t ctx_dim : ctx_candidates) {
                        if (!run_actor_probe(emb_dim, vnf_dim, ctx_dim)) {
                            continue;
                        }
                        embedding_dim_ = emb_dim;
                        vnf_feature_dim_ = vnf_dim;
                        context_feature_dim_ = ctx_dim;
                        actor_ok = true;
                        found = true;
                        break;
                    }
                    if (found) {
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
        }
        if (!actor_ok) {
            std::cerr << "[DRL Inference] Failed to probe compatible Actor input dimensions." << std::endl;
            return false;
        }

        std::cout << "[DRL Inference] Models loaded successfully"
                  << " (node_dim=" << node_feature_dim_
                  << ", emb_dim=" << embedding_dim_
                  << ", vnf_dim=" << vnf_feature_dim_
                  << ", context_dim=" << context_feature_dim_
                  << ")" << std::endl;
        return true;
    } catch (const Ort::Exception& e) {
        std::cerr << "[DRL Inference] ONNX Runtime error: " << e.what() << std::endl;
        std::cerr << "[DRL Inference] Model compatibility check failed." << std::endl;
        return false;
    }
}

std::vector<float> DRLInference::encode_graph(
    const std::vector<float>& node_features,
    const std::vector<int64_t>& edge_index,
    size_t num_nodes) {
    try {
        if (!gnn_session_) {
            std::cerr << "[DRL Inference] GNN session is not initialized." << std::endl;
            return {};
        }
        if (num_nodes == 0) {
            return {};
        }
        if (node_features.size() % num_nodes != 0) {
            std::cerr << "[DRL Inference] Node feature size mismatch: got " << node_features.size()
                      << ", not divisible by num_nodes " << num_nodes << std::endl;
            return {};
        }

        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const auto push_unique = [](std::vector<int64_t>& values, int64_t value) {
            if (value <= 0) {
                return;
            }
            if (std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(value);
            }
        };

        std::vector<int64_t> node_dim_candidates;
        push_unique(node_dim_candidates, node_feature_dim_);
        push_unique(node_dim_candidates, 18);
        push_unique(node_dim_candidates, 14);
        push_unique(node_dim_candidates, 8);

        std::string last_error;
        for (int64_t candidate_dim : node_dim_candidates) {
            try {
                std::vector<float> aligned_node_features = align_node_features(
                    node_features, num_nodes, static_cast<size_t>(candidate_dim));
                if (aligned_node_features.empty()) {
                    continue;
                }

                std::vector<int64_t> node_shape = {static_cast<int64_t>(num_nodes), candidate_dim};
                std::vector<int64_t> edge_shape = {2, static_cast<int64_t>(edge_index.size() / 2)};

                std::vector<Ort::Value> input_tensors;
                input_tensors.push_back(Ort::Value::CreateTensor<float>(
                    memory_info,
                    aligned_node_features.data(),
                    aligned_node_features.size(),
                    node_shape.data(),
                    node_shape.size()));
                input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
                    memory_info,
                    const_cast<int64_t*>(edge_index.data()),
                    edge_index.size(),
                    edge_shape.data(),
                    edge_shape.size()));

                const char* input_names[] = {"node_features", "edge_index"};
                const char* output_names[] = {"node_embeddings"};

                auto output_tensors = gnn_session_->Run(
                    Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 2, output_names, 1);

                node_feature_dim_ = candidate_dim;
                float* output_data = output_tensors[0].GetTensorMutableData<float>();
                auto output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
                size_t output_size = std::accumulate(output_shape.begin(), output_shape.end(), 1LL, std::multiplies<int64_t>());

                if (output_size == 0 || output_size % num_nodes != 0) {
                    std::cerr << "[DRL Inference] Invalid GNN output size: " << output_size
                              << " for num_nodes=" << num_nodes << std::endl;
                    return {};
                }

                const size_t output_dim = output_size / num_nodes;
                std::vector<float> raw_embeddings(output_data, output_data + output_size);
                if (output_dim == static_cast<size_t>(embedding_dim_)) {
                    return raw_embeddings;
                }

                std::vector<float> aligned_embeddings(num_nodes * static_cast<size_t>(embedding_dim_), 0.0f);
                const size_t copy_dim = std::min(output_dim, static_cast<size_t>(embedding_dim_));
                for (size_t i = 0; i < num_nodes; ++i) {
                    const float* src = raw_embeddings.data() + i * output_dim;
                    float* dst = aligned_embeddings.data() + i * static_cast<size_t>(embedding_dim_);
                    std::copy_n(src, copy_dim, dst);
                }
                std::cerr << "[DRL Inference] Node embedding adapt: gnn_dim=" << output_dim
                          << ", actor_dim=" << embedding_dim_
                          << " (truncate/pad applied)" << std::endl;
                return aligned_embeddings;
            } catch (const Ort::Exception& e) {
                last_error = e.what();
            }
        }

        if (!last_error.empty()) {
            std::cerr << "[DRL Inference] GNN encoding error: " << last_error << std::endl;
        }
        return {};

    } catch (const Ort::Exception& e) {
        std::cerr << "[DRL Inference] GNN encoding error: " << e.what() << std::endl;
        return {};
    }
}

int DRLInference::select_action(
    const std::vector<float>& node_embeddings,
    const std::vector<int>& candidate_indices,
    const std::vector<float>& vnf_features,
    const std::vector<float>& context_features) {
    try {
        if (!actor_session_) {
            std::cerr << "[DRL Inference] Actor session is not initialized." << std::endl;
            return 0;
        }
        if (candidate_indices.empty()) {
            return 0;
        }
        const auto push_unique = [](std::vector<int64_t>& values, int64_t value) {
            if (value <= 0) {
                return;
            }
            if (std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(value);
            }
        };

        std::vector<int64_t> emb_candidates;
        std::vector<int64_t> vnf_candidates;
        std::vector<int64_t> ctx_candidates;
        push_unique(emb_candidates, embedding_dim_);
        push_unique(emb_candidates, 192);
        push_unique(vnf_candidates, vnf_feature_dim_);
        push_unique(vnf_candidates, 24);
        push_unique(vnf_candidates, 8);
        push_unique(vnf_candidates, 4);
        push_unique(ctx_candidates, context_feature_dim_);
        push_unique(ctx_candidates, 32);
        push_unique(ctx_candidates, 48);

        std::vector<int64_t> candidates_int64(candidate_indices.begin(), candidate_indices.end());
        const size_t num_candidates = candidate_indices.size();
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::string last_error;

        for (int64_t emb_dim : emb_candidates) {
            if (node_embeddings.size() % static_cast<size_t>(emb_dim) != 0) {
                continue;
            }
            const size_t num_nodes = node_embeddings.size() / static_cast<size_t>(emb_dim);
            bool candidate_ok = true;
            for (int idx : candidate_indices) {
                if (idx < 0 || static_cast<size_t>(idx) >= num_nodes) {
                    candidate_ok = false;
                    break;
                }
            }
            if (!candidate_ok) {
                continue;
            }

            for (int64_t vnf_dim : vnf_candidates) {
                for (int64_t ctx_dim : ctx_candidates) {
                    try {
                        std::vector<float> aligned_vnf = align_flat_features(
                            vnf_features, static_cast<size_t>(vnf_dim), "vnf_features");
                        std::vector<float> aligned_context = align_flat_features(
                            context_features, static_cast<size_t>(ctx_dim), "context_features");
                        if (aligned_vnf.empty() || aligned_context.empty()) {
                            continue;
                        }

                        std::vector<int64_t> emb_shape = {static_cast<int64_t>(num_nodes), emb_dim};
                        std::vector<int64_t> cand_shape = {static_cast<int64_t>(num_candidates)};
                        std::vector<int64_t> vnf_shape = {vnf_dim};
                        std::vector<int64_t> ctx_shape = {ctx_dim};

                        std::vector<Ort::Value> input_tensors;
                        input_tensors.push_back(Ort::Value::CreateTensor<float>(
                            memory_info,
                            const_cast<float*>(node_embeddings.data()),
                            node_embeddings.size(),
                            emb_shape.data(),
                            emb_shape.size()));
                        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
                            memory_info,
                            candidates_int64.data(),
                            candidates_int64.size(),
                            cand_shape.data(),
                            cand_shape.size()));
                        input_tensors.push_back(Ort::Value::CreateTensor<float>(
                            memory_info,
                            aligned_vnf.data(),
                            aligned_vnf.size(),
                            vnf_shape.data(),
                            vnf_shape.size()));
                        input_tensors.push_back(Ort::Value::CreateTensor<float>(
                            memory_info,
                            aligned_context.data(),
                            aligned_context.size(),
                            ctx_shape.data(),
                            ctx_shape.size()));

                        const char* input_names[] = {"node_embeddings", "candidate_indices", "vnf_features", "context_features"};
                        const char* output_names[] = {"probs", "logits"};
                        auto output_tensors = actor_session_->Run(
                            Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 4, output_names, 2);

                        embedding_dim_ = emb_dim;
                        vnf_feature_dim_ = vnf_dim;
                        context_feature_dim_ = ctx_dim;
                        float* probs = output_tensors[0].GetTensorMutableData<float>();
                        int best_action = std::distance(probs, std::max_element(probs, probs + num_candidates));
                        return best_action;
                    } catch (const Ort::Exception& e) {
                        last_error = e.what();
                    }
                }
            }
        }

        if (!last_error.empty()) {
            std::cerr << "[DRL Inference] Actor inference error: " << last_error << std::endl;
        } else {
            std::cerr << "[DRL Inference] Actor inference failed: no compatible input dimensions found." << std::endl;
        }
        return 0;

    } catch (const Ort::Exception& e) {
        std::cerr << "[DRL Inference] Actor inference error: " << e.what() << std::endl;
        return 0;
    }
}

} // namespace sfc
