#pragma once
#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>

namespace sfc {

class DRLInference {
public:
    DRLInference();
    ~DRLInference();
    
    bool load_models(const std::string& gnn_path, const std::string& actor_path);
    
    std::vector<float> encode_graph(
        const std::vector<float>& node_features,
        const std::vector<int64_t>& edge_index,
        size_t num_nodes
    );
    
    int select_action(
        const std::vector<float>& node_embeddings,
        const std::vector<int>& candidate_indices,
        const std::vector<float>& vnf_features,
        const std::vector<float>& context_features
    );
    
private:
    static constexpr int64_t kExpectedNodeFeatureDim = 14;
    static constexpr int64_t kExpectedEmbeddingDim = 192;
    static constexpr int64_t kExpectedVnfFeatureDim = 8;
    static constexpr int64_t kExpectedContextFeatureDim = 48;

    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> gnn_session_;
    std::unique_ptr<Ort::Session> actor_session_;
    Ort::SessionOptions session_options_;
    Ort::AllocatorWithDefaultOptions allocator_;
};

} // namespace sfc
