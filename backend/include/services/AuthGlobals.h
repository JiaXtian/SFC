#pragma once

#include <memory>

namespace sfc {

class AuthService;
class UserService;

extern std::shared_ptr<AuthService> g_auth_service;
extern std::shared_ptr<UserService> g_user_service;

}  // namespace sfc
