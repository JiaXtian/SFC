#pragma once

#include <memory>

namespace sfc {

class AuthService;
class UserService;
class RuntimeStateService;
class DeploymentOrchestratorService;

extern std::shared_ptr<AuthService> g_auth_service;
extern std::shared_ptr<UserService> g_user_service;
extern std::shared_ptr<RuntimeStateService> g_runtime_state_service;
extern std::shared_ptr<DeploymentOrchestratorService> g_deployment_orchestrator;

}  // namespace sfc
