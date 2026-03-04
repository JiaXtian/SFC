#include "drl_inference.h"
#include "heuristic_pruner.h"
#include "network_graph.h"
#include "sfc_orchestrator.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <string>
#include <vector>

using namespace sfc;
namespace fs = std::filesystem;

struct Config {
    std::string gnn_model;
    std::string actor_model;
    std::string topology_file;
    std::string requests_file;
    std::string topology_dir;
    std::string requests_dir;
    std::string output_file = "result/results.json";
    int num_threads = 4;
    HeuristicConfig heuristic;
};

Config parse_args(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; i += 2) {
        std::string arg = argv[i];
        if (i + 1 >= argc) {
            break;
        }
        std::string value = argv[i + 1];

        if (arg == "--gnn_model") {
            config.gnn_model = value;
        } else if (arg == "--actor_model") {
            config.actor_model = value;
        } else if (arg == "--topology") {
            config.topology_file = value;
        } else if (arg == "--requests") {
            config.requests_file = value;
        } else if (arg == "--topology_dir") {
            config.topology_dir = value;
        } else if (arg == "--requests_dir") {
            config.requests_dir = value;
        } else if (arg == "--output") {
            config.output_file = value;
        } else if (arg == "--num_threads") {
            config.num_threads = std::stoi(value);
        } else if (arg == "--w_res") {
            config.heuristic.w_res = std::stof(value);
        } else if (arg == "--w_lat") {
            config.heuristic.w_lat = std::stof(value);
        } else if (arg == "--alpha") {
            config.heuristic.alpha = std::stof(value);
        } else if (arg == "--top_m") {
            config.heuristic.top_m = std::stoi(value);
        }
    }

    return config;
}

std::vector<std::string> list_json_files(const std::string& dir) {
    std::vector<std::string> files;
    if (dir.empty() || !fs::exists(dir)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

int read_topology_scale(const std::string& topo_file) {
    try {
        std::ifstream in(topo_file);
        nlohmann::json topo_json;
        in >> topo_json;
        return topo_json["metadata"].value("total_satellites", 0);
    } catch (...) {
        return 0;
    }
}

std::string read_request_topology_name(const std::string& req_file) {
    try {
        std::ifstream in(req_file);
        nlohmann::json req_json;
        in >> req_json;
        return req_json["metadata"].value("topology_file", "");
    } catch (...) {
        return "";
    }
}

std::string topology_scale_bucket(int scale) {
    if (scale <= 0) {
        return "unknown";
    }
    if (scale <= 1200) {
        return "small(<=1200)";
    }
    if (scale <= 3200) {
        return "medium(1201-3200)";
    }
    return "large(>3200)";
}

nlohmann::json path_to_json(const std::vector<std::string>& path) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& node : path) {
        arr.push_back(node);
    }
    return arr;
}

nlohmann::json attempts_to_json(const std::vector<CandidateAttemptTrace>& attempts) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& attempt : attempts) {
        arr.push_back({
            {"node_id", attempt.node_id},
            {"has_resources", attempt.has_resources},
            {"path_found", attempt.path_found},
            {"bandwidth_ok", attempt.bandwidth_ok},
            {"delay_ok", attempt.delay_ok},
            {"reliability_ok", attempt.reliability_ok},
            {"path_delay_ms", attempt.path_delay_ms},
            {"processing_delay_ms", attempt.processing_delay_ms},
            {"projected_total_delay_ms", attempt.projected_total_delay_ms},
            {"projected_reliability", attempt.projected_reliability},
            {"path", path_to_json(attempt.path)},
            {"reject_reason", attempt.reject_reason},
        });
    }
    return arr;
}

nlohmann::json vnf_traces_to_json(const std::vector<VNFDeploymentTrace>& traces) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& trace : traces) {
        arr.push_back({
            {"vnf_id", trace.vnf_id},
            {"vnf_type", trace.vnf_type},
            {"remaining_delay_before_ms", trace.remaining_delay_before_ms},
            {"accumulated_delay_before_ms", trace.accumulated_delay_before_ms},
            {"accumulated_reliability_before", trace.accumulated_reliability_before},
            {"candidate_count", trace.candidate_count},
            {"actor_choice_index", trace.actor_choice_index},
            {"actor_choice_node", trace.actor_choice_node},
            {"attempts", attempts_to_json(trace.attempts)},
            {"success", trace.success},
            {"selected_node", trace.selected_node},
            {"selected_path", path_to_json(trace.selected_path)},
            {"selected_path_delay_ms", trace.selected_path_delay_ms},
            {"selected_processing_delay_ms", trace.selected_processing_delay_ms},
            {"accumulated_delay_after_ms", trace.accumulated_delay_after_ms},
            {"accumulated_reliability_after", trace.accumulated_reliability_after},
            {"failure_reason", trace.failure_reason},
        });
    }
    return arr;
}

void print_top_failure_reasons(const std::unordered_map<std::string, int>& counts, const std::string& label) {
    std::vector<std::pair<std::string, int>> items(counts.begin(), counts.end());
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;
    });

    std::cout << label << std::endl;
    if (items.empty()) {
        std::cout << "  - 无失败记录" << std::endl;
        return;
    }
    const size_t limit = std::min<size_t>(5, items.size());
    for (size_t i = 0; i < limit; ++i) {
        std::cout << "  - " << items[i].second << " : " << items[i].first << std::endl;
    }
}

struct ScaleAggregate {
    int topology_count = 0;
    int total_requests = 0;
    int success_count = 0;
    float success_delay_sum = 0.0f;
    long long total_time_ms = 0;
    std::unordered_map<std::string, int> failure_reason_counts;
};

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "  SFC智能编排系统 - 星上推理" << std::endl;
    std::cout << "========================================" << std::endl;

    Config config = parse_args(argc, argv);

    if (config.gnn_model.empty() || config.actor_model.empty()) {
        std::cerr << "Error: --gnn_model and --actor_model are required" << std::endl;
        return 1;
    }

    std::vector<std::pair<std::string, std::string>> test_pairs;
    if (!config.topology_dir.empty() && !config.requests_dir.empty()) {
        auto topo_files = list_json_files(config.topology_dir);
        auto req_files = list_json_files(config.requests_dir);
        std::unordered_map<std::string, std::string> topo_by_name;
        for (const auto& topo_file : topo_files) {
            topo_by_name[fs::path(topo_file).filename().string()] = topo_file;
        }
        for (const auto& req_file : req_files) {
            const std::string topo_name = read_request_topology_name(req_file);
            auto it = topo_by_name.find(topo_name);
            if (it != topo_by_name.end()) {
                test_pairs.emplace_back(it->second, req_file);
            }
        }
        if (test_pairs.empty()) {
            size_t n = std::min(topo_files.size(), req_files.size());
            for (size_t i = 0; i < n; ++i) {
                test_pairs.emplace_back(topo_files[i], req_files[i]);
            }
        }
    } else if (!config.topology_file.empty() && !config.requests_file.empty()) {
        test_pairs.emplace_back(config.topology_file, config.requests_file);
    } else {
        std::cerr << "Error: either (--topology + --requests) or (--topology_dir + --requests_dir) is required" << std::endl;
        return 1;
    }

    std::cout << "配置:" << std::endl;
    std::cout << "  GNN模型: " << config.gnn_model << std::endl;
    std::cout << "  Actor模型: " << config.actor_model << std::endl;
    std::cout << "  测试拓扑数: " << test_pairs.size() << std::endl;
    std::cout << "  输出文件: " << config.output_file << std::endl;
    std::cout << "========================================" << std::endl;

    auto drl = std::make_shared<DRLInference>();
    if (!drl->load_models(config.gnn_model, config.actor_model)) {
        std::cerr << "Failed to load DRL models" << std::endl;
        return 1;
    }
    auto pruner = std::make_shared<HeuristicPruner>(config.heuristic);
    SFCOrchestrator orchestrator(pruner, drl);

    auto all_start = std::chrono::high_resolution_clock::now();

    int overall_total_requests = 0;
    int overall_success_count = 0;
    float overall_success_delay = 0.0f;
    nlohmann::json topology_results = nlohmann::json::array();
    std::unordered_map<std::string, int> overall_failure_reason_counts;
    std::map<std::string, ScaleAggregate> scale_aggregates;

    for (size_t t = 0; t < test_pairs.size(); ++t) {
        const auto& topo_file = test_pairs[t].first;
        const auto& req_file = test_pairs[t].second;
        const int topology_scale = read_topology_scale(topo_file);
        const std::string scale_bucket = topology_scale_bucket(topology_scale);

        std::cout << "\n[拓扑 " << (t + 1) << "/" << test_pairs.size() << "] " << topo_file << std::endl;
        std::cout << "  规模: " << topology_scale << " 节点, 分组: " << scale_bucket << std::endl;

        NetworkGraph graph;
        if (!graph.load_from_json(topo_file)) {
            std::cerr << "Failed to load topology: " << topo_file << std::endl;
            continue;
        }

        std::vector<SFCRequest> requests;
        try {
            requests = load_sfc_requests(req_file);
        } catch (const std::exception& e) {
            std::cerr << "Failed to load requests: " << req_file << " error=" << e.what() << std::endl;
            continue;
        }

        auto topo_start = std::chrono::high_resolution_clock::now();

        auto node_features = graph.get_node_features();
        auto edge_index = graph.get_edge_index();
        auto node_embeddings = drl->encode_graph(node_features, edge_index, graph.get_nodes().size());

        int topo_success_count = 0;
        float topo_success_delay = 0.0f;
        nlohmann::json per_sfc = nlohmann::json::array();
        std::unordered_map<std::string, int> topo_failure_reason_counts;

        for (const auto& request : requests) {
            overall_total_requests += 1;

            NetworkGraph request_graph = graph.clone();
            const bool verbose_sfc_log = (t + 1 == test_pairs.size());
            auto result = orchestrator.deploy_sfc(request_graph, request, node_embeddings, verbose_sfc_log);

            nlohmann::json sfc_json;
            sfc_json["request_id"] = result.request_id;
            sfc_json["service_type"] = request.service_type;
            sfc_json["source_node"] = request.source_node;
            sfc_json["destination_node"] = request.destination_node;
            sfc_json["vnf_count"] = request.vnf_sequence.size();
            nlohmann::json vnf_types = nlohmann::json::array();
            for (const auto& vnf : request.vnf_sequence) {
                vnf_types.push_back(vnf.vnf_type);
            }
            sfc_json["vnf_types"] = vnf_types;
            sfc_json["success"] = result.success;
            sfc_json["total_delay_ms"] = result.total_delay_ms;
            sfc_json["final_reliability"] = result.final_reliability;
            sfc_json["deployed_nodes"] = result.deployed_nodes;
            nlohmann::json paths_json = nlohmann::json::array();
            for (const auto& path : result.paths) {
                paths_json.push_back(path_to_json(path));
            }
            sfc_json["paths"] = paths_json;
            sfc_json["failure_reason"] = result.failure_reason;
            sfc_json["vnf_traces"] = vnf_traces_to_json(result.vnf_traces);
            per_sfc.push_back(sfc_json);

            if (result.success) {
                topo_success_count += 1;
                overall_success_count += 1;
                topo_success_delay += result.total_delay_ms;
                overall_success_delay += result.total_delay_ms;
            } else {
                topo_failure_reason_counts[result.failure_reason] += 1;
                overall_failure_reason_counts[result.failure_reason] += 1;
            }
        }

        auto topo_end = std::chrono::high_resolution_clock::now();
        auto topo_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(topo_end - topo_start).count();

        int topo_total_requests = static_cast<int>(requests.size());
        float topo_success_rate = topo_total_requests > 0 ? 100.0f * topo_success_count / topo_total_requests : 0.0f;
        float topo_avg_delay = topo_success_count > 0 ? topo_success_delay / topo_success_count : 0.0f;

        std::cout << "  拓扑统计: req=" << topo_total_requests
                  << " succ=" << topo_success_count
                  << " rate=" << topo_success_rate << "%"
                  << " avg_delay=" << topo_avg_delay << "ms"
                  << " time=" << topo_time_ms << "ms" << std::endl;
        print_top_failure_reasons(topo_failure_reason_counts, "  失败原因Top:");

        nlohmann::json topo_json;
        topo_json["topology_file"] = topo_file;
        topo_json["requests_file"] = req_file;
        topo_json["topology_scale"] = topology_scale;
        topo_json["scale_bucket"] = scale_bucket;
        topo_json["total_requests"] = topo_total_requests;
        topo_json["success_count"] = topo_success_count;
        topo_json["success_rate"] = topo_total_requests > 0 ? static_cast<float>(topo_success_count) / topo_total_requests : 0.0f;
        topo_json["average_delay_ms"] = topo_avg_delay;
        topo_json["total_time_ms"] = topo_time_ms;
        topo_json["failure_reason_counts"] = topo_failure_reason_counts;
        topo_json["sfc_results"] = per_sfc;
        topology_results.push_back(topo_json);

        auto& scale_stats = scale_aggregates[scale_bucket];
        scale_stats.topology_count += 1;
        scale_stats.total_requests += topo_total_requests;
        scale_stats.success_count += topo_success_count;
        scale_stats.success_delay_sum += topo_success_delay;
        scale_stats.total_time_ms += topo_time_ms;
        for (const auto& [reason, count] : topo_failure_reason_counts) {
            scale_stats.failure_reason_counts[reason] += count;
        }
    }

    auto all_end = std::chrono::high_resolution_clock::now();
    auto total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(all_end - all_start).count();

    float overall_success_rate = overall_total_requests > 0 ? 100.0f * overall_success_count / overall_total_requests : 0.0f;
    float overall_avg_delay = overall_success_count > 0 ? overall_success_delay / overall_success_count : 0.0f;
    float avg_time_per_sfc = overall_total_requests > 0 ? static_cast<float>(total_time_ms) / overall_total_requests : 0.0f;

    std::cout << "\n========================================" << std::endl;
    std::cout << "  总体统计" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "拓扑总数: " << test_pairs.size() << std::endl;
    std::cout << "总请求数: " << overall_total_requests << std::endl;
    std::cout << "成功数: " << overall_success_count << " (" << overall_success_rate << "%)" << std::endl;
    std::cout << "平均成功时延: " << overall_avg_delay << " ms" << std::endl;
    std::cout << "总耗时: " << total_time_ms << " ms" << std::endl;
    std::cout << "平均每SFC处理时延: " << avg_time_per_sfc << " ms" << std::endl;
    std::cout << "========================================" << std::endl;
    print_top_failure_reasons(overall_failure_reason_counts, "总体失败原因Top:");

    std::cout << "\n========================================" << std::endl;
    std::cout << "  按拓扑规模聚合分析" << std::endl;
    std::cout << "========================================" << std::endl;

    nlohmann::json scale_analysis = nlohmann::json::array();
    for (const auto& [bucket, stats] : scale_aggregates) {
        const float bucket_rate = stats.total_requests > 0 ? 100.0f * stats.success_count / stats.total_requests : 0.0f;
        const float bucket_delay = stats.success_count > 0 ? stats.success_delay_sum / stats.success_count : 0.0f;
        const float bucket_time_per_sfc = stats.total_requests > 0 ? static_cast<float>(stats.total_time_ms) / stats.total_requests : 0.0f;

        std::cout << bucket
                  << ": topo=" << stats.topology_count
                  << " req=" << stats.total_requests
                  << " succ=" << stats.success_count
                  << " rate=" << bucket_rate << "%"
                  << " avg_delay=" << bucket_delay << "ms"
                  << " avg_time_per_sfc=" << bucket_time_per_sfc << "ms" << std::endl;
        print_top_failure_reasons(stats.failure_reason_counts, "  该规模失败原因Top:");

        scale_analysis.push_back({
            {"scale_bucket", bucket},
            {"topology_count", stats.topology_count},
            {"total_requests", stats.total_requests},
            {"success_count", stats.success_count},
            {"success_rate", stats.total_requests > 0 ? static_cast<float>(stats.success_count) / stats.total_requests : 0.0f},
            {"average_delay_ms", bucket_delay},
            {"avg_time_per_sfc_ms", bucket_time_per_sfc},
            {"failure_reason_counts", stats.failure_reason_counts},
        });
    }
    std::cout << "========================================" << std::endl;

    nlohmann::json output;
    output["overall"] = {
        {"total_topologies", test_pairs.size()},
        {"total_requests", overall_total_requests},
        {"success_count", overall_success_count},
        {"success_rate", overall_total_requests > 0 ? static_cast<float>(overall_success_count) / overall_total_requests : 0.0f},
        {"average_delay_ms", overall_avg_delay},
        {"total_time_ms", total_time_ms},
        {"avg_time_per_sfc_ms", avg_time_per_sfc},
        {"failure_reason_counts", overall_failure_reason_counts},
    };
    output["scale_analysis"] = scale_analysis;
    output["topologies"] = topology_results;

    fs::path out_path(config.output_file);
    if (!out_path.parent_path().empty()) {
        fs::create_directories(out_path.parent_path());
    }
    std::ofstream out(config.output_file);
    out << output.dump(2);
    out.close();

    std::cout << "\n✓ 结果已保存到: " << config.output_file << std::endl;
    return 0;
}
