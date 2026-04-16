#include "websocket/WSHandler.h"
#include "services/AuthGlobals.h"
#include "services/AuthService.h"
#include <spdlog/spdlog.h>

namespace sfc {

std::set<WebSocketConnectionPtr> WSHandler::connections_{};
std::mutex WSHandler::connections_mutex_{};

void WSHandler::handleNewMessage(
    const WebSocketConnectionPtr& wsConnPtr,
    std::string&& message,
    const WebSocketMessageType& type
) {
    (void)type;
    spdlog::debug("WebSocket message received: {}", message);
    
    // Echo back
    wsConnPtr->send(message);
}

void WSHandler::handleNewConnection(
    const HttpRequestPtr& req,
    const WebSocketConnectionPtr& wsConnPtr
) {
    const auto token_param = req->getParameter("token");
    std::string token = token_param;
    if (token.empty()) {
        const auto auth = req->getHeader("Authorization");
        constexpr const char* kPrefix = "Bearer ";
        if (auth.size() > 7 && auth.rfind(kPrefix, 0) == 0) {
            token = auth.substr(7);
        }
    }

    if (!g_auth_service || token.empty() || !g_auth_service->verify_token(token)) {
        spdlog::warn("WebSocket rejected: unauthorized {}", wsConnPtr->peerAddr().toIpPort());
        wsConnPtr->shutdown();
        return;
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.insert(wsConnPtr);
    
    spdlog::info("WebSocket connected: {} (total: {})",
                wsConnPtr->peerAddr().toIpPort(),
                connections_.size());
}

void WSHandler::handleConnectionClosed(const WebSocketConnectionPtr& wsConnPtr) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.erase(wsConnPtr);
    
    spdlog::info("WebSocket disconnected: {} (total: {})",
                wsConnPtr->peerAddr().toIpPort(),
                connections_.size());
}

void WSHandler::broadcast(const std::string& message) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    
    for (const auto& conn : connections_) {
        conn->send(message);
    }
    
    spdlog::debug("Broadcasted to {} connections", connections_.size());
}

void WSHandler::broadcast_json(const nlohmann::json& payload) {
    broadcast(payload.dump());
}

} // namespace sfc
