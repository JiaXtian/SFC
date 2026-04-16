#include "controllers/AuthController.h"

#include "services/AuthGlobals.h"
#include "services/AuthService.h"
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

std::string extract_bearer_token(const HttpRequestPtr& req) {
    const auto auth = req->getHeader("Authorization");
    constexpr const char* kPrefix = "Bearer ";
    if (auth.size() <= 7 || auth.rfind(kPrefix, 0) != 0) return {};
    return trim(auth.substr(7));
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

}  // namespace

void AuthController::login(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback) {
    auto json = req->getJsonObject();
    if (!json) {
        respond_json(callback, build_error(400, "Invalid JSON"), k400BadRequest);
        return;
    }
    if (!g_user_service || !g_auth_service) {
        respond_json(callback, build_error(500, "Auth service unavailable"), k500InternalServerError);
        return;
    }

    const std::string username = trim((*json).get("username", "").asString());
    const std::string password = (*json).get("password", "").asString();
    if (username.empty() || password.empty()) {
        respond_json(callback, build_error(400, "username and password are required"), k400BadRequest);
        return;
    }

    const auto user_opt = g_user_service->find_by_username(username);
    if (!user_opt || !AuthService::verify_password(password, user_opt->password_hash)) {
        respond_json(callback, build_error(401, "invalid_credentials"), k401Unauthorized);
        return;
    }

    Json::Value resp;
    resp["token"] = g_auth_service->issue_token(user_opt->id, user_opt->username, user_opt->role);
    resp["expires_in_hours"] = g_auth_service->token_expire_hours();
    resp["user"] = user_to_json(*user_opt);
    respond_json(callback, resp);
}

void AuthController::register_user(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback) {
    auto json = req->getJsonObject();
    if (!json) {
        respond_json(callback, build_error(400, "Invalid JSON"), k400BadRequest);
        return;
    }
    if (!g_user_service || !g_auth_service) {
        respond_json(callback, build_error(500, "Auth service unavailable"), k500InternalServerError);
        return;
    }

    const std::string username = trim((*json).get("username", "").asString());
    const std::string password = (*json).get("password", "").asString();
    std::string confirm_password = (*json).get("confirm_password", "").asString();
    if (confirm_password.empty()) {
        confirm_password = (*json).get("confirmPassword", "").asString();
    }

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
    if (password != confirm_password) {
        respond_json(callback, build_error(400, "password_mismatch"), k400BadRequest);
        return;
    }

    int64_t inserted_id = 0;
    std::string error_message;
    if (!g_user_service->create_user(username, password, "user", &error_message, &inserted_id)) {
        const auto status =
            error_message == "username_exists" ? k409Conflict : k500InternalServerError;
        respond_json(callback, build_error(static_cast<int>(status), error_message), status);
        return;
    }

    const auto new_user = g_user_service->find_by_id(inserted_id);
    if (!new_user) {
        respond_json(callback, build_error(500, "register_success_but_user_not_found"), k500InternalServerError);
        return;
    }

    Json::Value resp;
    resp["token"] = g_auth_service->issue_token(new_user->id, new_user->username, new_user->role);
    resp["expires_in_hours"] = g_auth_service->token_expire_hours();
    resp["user"] = user_to_json(*new_user);
    respond_json(callback, resp, k201Created);
}

void AuthController::me(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!g_auth_service || !g_user_service) {
        respond_json(callback, build_error(500, "Auth service unavailable"), k500InternalServerError);
        return;
    }
    const std::string token = extract_bearer_token(req);
    if (token.empty()) {
        respond_json(callback, build_error(401, "missing_token"), k401Unauthorized);
        return;
    }
    const auto claims = g_auth_service->verify_token(token);
    if (!claims) {
        respond_json(callback, build_error(401, "invalid_token"), k401Unauthorized);
        return;
    }

    const auto user_opt = g_user_service->find_by_id(claims->user_id);
    if (!user_opt) {
        respond_json(callback, build_error(401, "user_not_found"), k401Unauthorized);
        return;
    }

    Json::Value resp;
    resp["user"] = user_to_json(*user_opt);
    respond_json(callback, resp);
}

}  // namespace sfc
