#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

namespace sfc {

class UserController : public HttpController<UserController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UserController::list_users, "/api/v1/users", Get);
    ADD_METHOD_TO(UserController::create_user, "/api/v1/users", Post);
    ADD_METHOD_TO(UserController::update_user, "/api/v1/users/{1}", Put);
    ADD_METHOD_TO(UserController::delete_user, "/api/v1/users/{1}", Delete);
    METHOD_LIST_END

    void list_users(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

    void create_user(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);

    void update_user(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& user_id);

    void delete_user(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& user_id);
};

}  // namespace sfc
