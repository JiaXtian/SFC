#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace sfc {

// Patch deployment record identified by deployment_id or backend_deployment_id.
// Returns false if deployment not found.
bool patch_deployment_record(
    const std::string& deployment_id,
    const nlohmann::json& patch,
    nlohmann::json* merged_out = nullptr
);

nlohmann::json list_deployment_records();

}  // namespace sfc

