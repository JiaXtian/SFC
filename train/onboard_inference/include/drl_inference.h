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
    static constexpr int64_t kDefaultNodeFeatureDim = 14;
    static constexpr int64_t kDefaultEmbeddingDim = 192;
    static constexpr int64_t kDefaultVnfFeatureDim = 8;
    static constexpr int64_t kDefaultContextFeatureDim = 48;

    std::vector<float> align_flat_features(
        const std::vector<float>& features,
        size_t target_dim,
        const char* feature_name
    ) const;
    std::vector<float> align_node_features(
        const std::vector<float>& node_features,
        size_t num_nodes
    ) const;

    int64_t node_feature_dim_ = kDefaultNodeFeatureDim;
    int64_t embedding_dim_ = kDefaultEmbeddingDim;
    int64_t vnf_feature_dim_ = kDefaultVnfFeatureDim;
    int64_t context_feature_dim_ = kDefaultContextFeatureDim;

    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> gnn_session_;
    std::unique_ptr<Ort::Session> actor_session_;
    Ort::SessionOptions session_options_;
    Ort::AllocatorWithDefaultOptions allocator_;
};

} // namespace sfc
