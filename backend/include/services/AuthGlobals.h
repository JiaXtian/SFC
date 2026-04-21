#pragma once

#include <memory>

namespace sfc {

class AuthService;
class UserService;
class RuntimeStateService;

extern std::shared_ptr<AuthService> g_auth_service;
extern std::shared_ptr<UserService> g_user_service;
extern std::shared_ptr<RuntimeStateService> g_runtime_state_service;

}  // namespace sfc
