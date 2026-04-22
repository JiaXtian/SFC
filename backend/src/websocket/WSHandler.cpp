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

    bool logged_in = false;
    if (!token.empty() && g_auth_service && g_auth_service->verify_token(token)) {
        logged_in = true;
    } else if (!token.empty()) {
        spdlog::warn("WebSocket token verification failed, fallback to guest: {}", wsConnPtr->peerAddr().toIpPort());
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.insert(wsConnPtr);
    
    spdlog::info("WebSocket connected: {} (total: {}, mode={})",
                wsConnPtr->peerAddr().toIpPort(),
                connections_.size(),
                logged_in ? "auth" : "guest");
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
