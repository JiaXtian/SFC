#pragma once
#include <json/json.h>
#include <nlohmann/json.hpp>

namespace sfc {

// 转换 nlohmann::json 到 Json::Value
inline Json::Value nlohmann_to_jsoncpp(const nlohmann::json& nj) {
    Json::Value jv;
    Json::Reader reader;
    reader.parse(nj.dump(), jv);
    return jv;
}

} // namespace sfc
