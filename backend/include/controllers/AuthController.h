#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

namespace sfc {

class AuthController : public HttpController<AuthController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::login, "/api/v1/auth/login", Post);
    ADD_METHOD_TO(AuthController::register_user, "/api/v1/auth/register", Post);
    ADD_METHOD_TO(AuthController::me, "/api/v1/auth/me", Get);
    METHOD_LIST_END

    void login(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback);

    void register_user(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);

    void me(const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback);
};

}  // namespace sfc
