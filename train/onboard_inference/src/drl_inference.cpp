#include "drl_inference.h"
#include <algorithm>
#include <iostream>
#include <numeric>

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

        std::cout << "[DRL Inference] Models loaded successfully" << std::endl;
        return true;
    } catch (const Ort::Exception& e) {
        std::cerr << "[DRL Inference] ONNX Runtime error: " << e.what() << std::endl;
        return false;
    }
}

std::vector<float> DRLInference::encode_graph(
    const std::vector<float>& node_features,
    const std::vector<int64_t>& edge_index,
    size_t num_nodes) {
    try {
        std::vector<int64_t> node_shape = {static_cast<int64_t>(num_nodes), 8};
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
        size_t num_nodes = node_embeddings.size() / 192;
        size_t num_candidates = candidate_indices.size();

        std::vector<int64_t> candidates_int64(candidate_indices.begin(), candidate_indices.end());

        std::vector<int64_t> emb_shape = {static_cast<int64_t>(num_nodes), 192};
        std::vector<int64_t> cand_shape = {static_cast<int64_t>(num_candidates)};
        std::vector<int64_t> vnf_shape = {4};
        std::vector<int64_t> ctx_shape = {48};

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
