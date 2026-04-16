#include "controllers/UserController.h"

#include "services/AuthGlobals.h"
#include "services/UserService.h"

#include <cctype>
#include <regex>

namespace sfc {

namespace {

Json::Value build_error(int code, const std::string& message, const std::string& details = "") {
    Json::Value err;
    err["code"] = code;
    err["message"] = message;
    if (!details.empty()) err["details"] = details;
    return err;
}

void respond_json(std::function<void(const HttpResponsePtr&)>& callback,
                  const Json::Value& value,
                  HttpStatusCode status = k200OK) {
    auto resp = HttpResponse::newHttpJsonResponse(value);
    resp->setStatusCode(status);
    callback(resp);
}

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

bool is_valid_username(const std::string& username) {
    static const std::regex kPattern("^[A-Za-z0-9_-]{3,32}$");
    return std::regex_match(username, kPattern);
}

bool is_valid_password(const std::string& password) {
    return password.size() >= 6 && password.size() <= 64;
}

Json::Value user_to_json(const UserRecord& user) {
    Json::Value out;
    out["id"] = static_cast<Json::Int64>(user.id);
    out["username"] = user.username;
    out["role"] = user.role;
    out["created_at"] = user.created_at;
    out["updated_at"] = user.updated_at;
    return out;
}

std::optional<int64_t> parse_user_id(const std::string& user_id) {
    try {
        const auto id = std::stoll(user_id);
        if (id <= 0) return std::nullopt;
        return id;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

void UserController::list_users(const HttpRequestPtr&,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!g_user_service) {
        respond_json(callback, build_error(500, "user_service_unavailable"), k500InternalServerError);
        return;
    }
    Json::Value users(Json::arrayValue);
    for (const auto& user : g_user_service->list_users()) {
        users.append(user_to_json(user));
    }
    Json::Value resp;
    resp["users"] = users;
    respond_json(callback, resp);
}

void UserController::create_user(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback) {
    auto json = req->getJsonObject();
    if (!json) {
        respond_json(callback, build_error(400, "Invalid JSON"), k400BadRequest);
        return;
    }
    if (!g_user_service) {
        respond_json(callback, build_error(500, "user_service_unavailable"), k500InternalServerError);
        return;
    }

    const std::string username = trim((*json).get("username", "").asString());
    const std::string password = (*json).get("password", "").asString();
    std::string role = trim((*json).get("role", "user").asString());
    if (role.empty()) role = "user";

    if (!is_valid_username(username)) {
        respond_json(
            callback,
            build_error(400, "invalid_username", "username must be 3-32 chars of letters, numbers, _ or -"),
            k400BadRequest);
        return;
    }
    if (!is_valid_password(password)) {
        respond_json(
            callback,
            build_error(400, "invalid_password", "password length must be between 6 and 64"),
            k400BadRequest);
        return;
    }
    if (role != "admin" && role != "user") {
        respond_json(callback, build_error(400, "invalid_role"), k400BadRequest);
        return;
    }

    std::string error_message;
    int64_t inserted_id = 0;
    if (!g_user_service->create_user(username, password, role, &error_message, &inserted_id)) {
        const auto status = error_message == "username_exists" ? k409Conflict : k500InternalServerError;
        respond_json(callback, build_error(static_cast<int>(status), error_message), status);
        return;
    }

    const auto created = g_user_service->find_by_id(inserted_id);
    if (!created) {
        respond_json(callback, build_error(500, "create_success_but_user_not_found"), k500InternalServerError);
        return;
    }

    Json::Value resp;
    resp["user"] = user_to_json(*created);
    respond_json(callback, resp, k201Created);
}

void UserController::update_user(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 const std::string& user_id) {
    auto json = req->getJsonObject();
    if (!json) {
        respond_json(callback, build_error(400, "Invalid JSON"), k400BadRequest);
        return;
    }
    if (!g_user_service) {
        respond_json(callback, build_error(500, "user_service_unavailable"), k500InternalServerError);
        return;
    }
    const auto id_opt = parse_user_id(user_id);
    if (!id_opt) {
        respond_json(callback, build_error(400, "invalid_user_id"), k400BadRequest);
        return;
    }

    std::optional<std::string> username;
    std::optional<std::string> password;
    std::optional<std::string> role;

    if (json->isMember("username")) {
        const std::string v = trim((*json)["username"].asString());
        if (!v.empty()) {
            if (!is_valid_username(v)) {
                respond_json(callback, build_error(400, "invalid_username"), k400BadRequest);
                return;
            }
            username = v;
        }
    }
    if (json->isMember("password")) {
        const std::string v = (*json)["password"].asString();
        if (!v.empty()) {
            if (!is_valid_password(v)) {
                respond_json(callback, build_error(400, "invalid_password"), k400BadRequest);
                return;
            }
            if (json->isMember("confirm_password")) {
                if (v != (*json)["confirm_password"].asString()) {
                    respond_json(callback, build_error(400, "password_mismatch"), k400BadRequest);
                    return;
                }
            }
            password = v;
        }
    }
    if (json->isMember("role")) {
        const std::string v = trim((*json)["role"].asString());
        if (!v.empty()) {
            if (v != "admin" && v != "user") {
                respond_json(callback, build_error(400, "invalid_role"), k400BadRequest);
                return;
            }
            role = v;
        }
    }

    if (!username && !password && !role) {
        respond_json(callback, build_error(400, "nothing_to_update"), k400BadRequest);
        return;
    }

    std::string error_message;
    if (!g_user_service->update_user(*id_opt, username, password, role, &error_message)) {
        HttpStatusCode status = k500InternalServerError;
        if (error_message == "user_not_found") status = k404NotFound;
        if (error_message == "username_exists") status = k409Conflict;
        if (error_message == "last_admin_forbidden") status = k400BadRequest;
        if (error_message == "invalid_role") status = k400BadRequest;
        respond_json(callback, build_error(static_cast<int>(status), error_message), status);
        return;
    }

    const auto updated = g_user_service->find_by_id(*id_opt);
    if (!updated) {
        respond_json(callback, build_error(404, "user_not_found"), k404NotFound);
        return;
    }

    Json::Value resp;
    resp["user"] = user_to_json(*updated);
    respond_json(callback, resp);
}

void UserController::delete_user(const HttpRequestPtr&,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 const std::string& user_id) {
    if (!g_user_service) {
        respond_json(callback, build_error(500, "user_service_unavailable"), k500InternalServerError);
        return;
    }
    const auto id_opt = parse_user_id(user_id);
    if (!id_opt) {
        respond_json(callback, build_error(400, "invalid_user_id"), k400BadRequest);
        return;
    }

    std::string error_message;
    if (!g_user_service->delete_user(*id_opt, &error_message)) {
        HttpStatusCode status = k500InternalServerError;
        if (error_message == "user_not_found") status = k404NotFound;
        if (error_message == "last_admin_forbidden") status = k400BadRequest;
        respond_json(callback, build_error(static_cast<int>(status), error_message), status);
        return;
    }

    Json::Value resp;
    resp["status"] = "deleted";
    resp["user_id"] = static_cast<Json::Int64>(*id_opt);
    respond_json(callback, resp);
}

}  // namespace sfc
