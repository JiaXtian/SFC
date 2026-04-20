#include "drl_inference.h"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <string>

namespace sfc {

DRLInference::DRLInference() {
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "SFC");
    session_options_.SetIntraOpNumThreads(1);
    session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
}

DRLInference::~DRLInference() = default;

bool DRLInference::load_models(const std::string& gnn_path, const std::string& actor_path) {
    try {
        gnn_session_ = std::make_unique<Ort::Session>(*env_, gnn_path.c_str(), session_options_);
        std::cout << "[DRL Inference] Loading GNN model: " << gnn_path << std::endl;

        actor_session_ = std::make_unique<Ort::Session>(*env_, actor_path.c_str(), session_options_);
        std::cout << "[DRL Inference] Loading Actor model: " << actor_path << std::endl;

        const auto get_last_dim = [](Ort::Session& session, size_t input_idx) -> int64_t {
            auto info = session.GetInputTypeInfo(input_idx).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (shape.empty()) {
                return -1;
            }
            return shape.back();
        };
        const auto check_dim = [](const char* tag, int64_t actual, int64_t expected) -> bool {
            if (actual == -1 || actual == expected) {
                return true;
            }
            std::cerr << "[DRL Inference] Model dimension mismatch for " << tag
                      << ": got " << actual << ", expected " << expected
                      << ". Please rerun training/export to regenerate ONNX models." << std::endl;
            return false;
        };

        if (gnn_session_->GetInputCount() < 1 || actor_session_->GetInputCount() < 4) {
            std::cerr << "[DRL Inference] Invalid model input signatures." << std::endl;
            return false;
        }

        const int64_t gnn_node_dim = get_last_dim(*gnn_session_, 0);
        const int64_t actor_emb_dim = get_last_dim(*actor_session_, 0);
        const int64_t actor_vnf_dim = get_last_dim(*actor_session_, 2);
        const int64_t actor_ctx_dim = get_last_dim(*actor_session_, 3);

        if (!check_dim("gnn.node_features", gnn_node_dim, kExpectedNodeFeatureDim) ||
            !check_dim("actor.node_embeddings", actor_emb_dim, kExpectedEmbeddingDim) ||
            !check_dim("actor.vnf_features", actor_vnf_dim, kExpectedVnfFeatureDim) ||
            !check_dim("actor.context_features", actor_ctx_dim, kExpectedContextFeatureDim)) {
            return false;
        }

        // Compatibility probe: catch symbolic-shape models that still mismatch expected dimensions.
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        {
            std::vector<float> probe_nodes(2 * static_cast<size_t>(kExpectedNodeFeatureDim), 0.0f);
            std::vector<int64_t> probe_edges = {0, 1, 1, 0};
            std::vector<int64_t> node_shape = {2, kExpectedNodeFeatureDim};
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
        }
        {
            std::vector<float> probe_emb(2 * static_cast<size_t>(kExpectedEmbeddingDim), 0.0f);
            std::vector<int64_t> probe_candidates = {0, 1};
            std::vector<float> probe_vnf(static_cast<size_t>(kExpectedVnfFeatureDim), 0.0f);
            std::vector<float> probe_ctx(static_cast<size_t>(kExpectedContextFeatureDim), 0.0f);
            std::vector<int64_t> emb_shape = {2, kExpectedEmbeddingDim};
            std::vector<int64_t> cand_shape = {2};
            std::vector<int64_t> vnf_shape = {kExpectedVnfFeatureDim};
            std::vector<int64_t> ctx_shape = {kExpectedContextFeatureDim};
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
        }

        std::cout << "[DRL Inference] Models loaded successfully" << std::endl;
        return true;
    } catch (const Ort::Exception& e) {
        std::cerr << "[DRL Inference] ONNX Runtime error: " << e.what() << std::endl;
        std::cerr << "[DRL Inference] Model compatibility check failed. "
                  << "Please rerun training/export to regenerate 14/8/48 dimension ONNX models."
                  << std::endl;
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
        if (node_features.size() != num_nodes * static_cast<size_t>(kExpectedNodeFeatureDim)) {
            std::cerr << "[DRL Inference] Node feature size mismatch: got " << node_features.size()
                      << ", expected " << (num_nodes * static_cast<size_t>(kExpectedNodeFeatureDim))
                      << std::endl;
            return {};
        }

        std::vector<int64_t> node_shape = {static_cast<int64_t>(num_nodes), kExpectedNodeFeatureDim};
        std::vector<int64_t> edge_shape = {2, static_cast<int64_t>(edge_index.size() / 2)};

        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<Ort::Value> input_tensors;
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info,
            const_cast<float*>(node_features.data()),
            node_features.size(),
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

        float* output_data = output_tensors[0].GetTensorMutableData<float>();
        auto output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t output_size = std::accumulate(output_shape.begin(), output_shape.end(), 1LL, std::multiplies<int64_t>());

        return std::vector<float>(output_data, output_data + output_size);

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
        if (vnf_features.size() != static_cast<size_t>(kExpectedVnfFeatureDim)) {
            std::cerr << "[DRL Inference] VNF feature size mismatch: got " << vnf_features.size()
                      << ", expected " << kExpectedVnfFeatureDim << std::endl;
            return 0;
        }
        if (context_features.size() != static_cast<size_t>(kExpectedContextFeatureDim)) {
            std::cerr << "[DRL Inference] Context feature size mismatch: got " << context_features.size()
                      << ", expected " << kExpectedContextFeatureDim << std::endl;
            return 0;
        }
        if (node_embeddings.size() % static_cast<size_t>(kExpectedEmbeddingDim) != 0) {
            std::cerr << "[DRL Inference] Node embedding size mismatch: got " << node_embeddings.size()
                      << ", expected multiple of " << kExpectedEmbeddingDim << std::endl;
            return 0;
        }

        size_t num_nodes = node_embeddings.size() / static_cast<size_t>(kExpectedEmbeddingDim);
        size_t num_candidates = candidate_indices.size();

        std::vector<int64_t> candidates_int64(candidate_indices.begin(), candidate_indices.end());

        std::vector<int64_t> emb_shape = {static_cast<int64_t>(num_nodes), kExpectedEmbeddingDim};
        std::vector<int64_t> cand_shape = {static_cast<int64_t>(num_candidates)};
        std::vector<int64_t> vnf_shape = {kExpectedVnfFeatureDim};
        std::vector<int64_t> ctx_shape = {kExpectedContextFeatureDim};

        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<Ort::Value> input_tensors;
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info,
            const_cast<float*>(node_embeddings.data()),
            node_embeddings.size(),
            emb_shape.data(),
            emb_shape.size()));
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
            memory_info,
            const_cast<int64_t*>(candidates_int64.data()),
            candidates_int64.size(),
            cand_shape.data(),
            cand_shape.size()));
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info,
            const_cast<float*>(vnf_features.data()),
            vnf_features.size(),
            vnf_shape.data(),
            vnf_shape.size()));
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info,
            const_cast<float*>(context_features.data()),
            context_features.size(),
            ctx_shape.data(),
            ctx_shape.size()));

        const char* input_names[] = {"node_embeddings", "candidate_indices", "vnf_features", "context_features"};
        const char* output_names[] = {"probs", "logits"};

        auto output_tensors = actor_session_->Run(
            Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 4, output_names, 2);

        float* probs = output_tensors[0].GetTensorMutableData<float>();
        int best_action = std::distance(probs, std::max_element(probs, probs + num_candidates));
        return best_action;

    } catch (const Ort::Exception& e) {
        std::cerr << "[DRL Inference] Actor inference error: " << e.what() << std::endl;
        return 0;
    }
}

} // namespace sfc
