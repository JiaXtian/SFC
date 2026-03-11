#pragma once
#include <drogon/WebSocketController.h>
#include <set>
#include <mutex>
#include <nlohmann/json.hpp>

using namespace drogon;

namespace sfc {

class WSHandler : public WebSocketController<WSHandler> {
public:
    void handleNewMessage(const WebSocketConnectionPtr& wsConnPtr,
                         std::string&& message,
                         const WebSocketMessageType& type) override;
    
    void handleNewConnection(const HttpRequestPtr& req,
                            const WebSocketConnectionPtr& wsConnPtr) override;
    
    void handleConnectionClosed(const WebSocketConnectionPtr& wsConnPtr) override;
    
    // 广播消息
    static void broadcast(const std::string& message);
    static void broadcast_json(const nlohmann::json& payload);
    
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/updates");
    WS_PATH_LIST_END
    
private:
    static std::set<WebSocketConnectionPtr> connections_;
    static std::mutex connections_mutex_;
};

} // namespace sfc
