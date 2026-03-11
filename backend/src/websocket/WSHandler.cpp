#include "websocket/WSHandler.h"
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
