#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

namespace sfc {

class UERANSIMController : public HttpController<UERANSIMController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UERANSIMController::listDeployments, "/api/v1/ueransim/deployments", Get);
    ADD_METHOD_TO(UERANSIMController::startVerification, "/api/v1/ueransim/verification/start", Post);
    ADD_METHOD_TO(UERANSIMController::getVerification, "/api/v1/ueransim/verification/{job_id}", Get);
    ADD_METHOD_TO(UERANSIMController::sendMessage, "/api/v1/ueransim/verification/{job_id}/messages", Post);
    ADD_METHOD_TO(UERANSIMController::stopVerification, "/api/v1/ueransim/verification/{job_id}/stop", Post);
    METHOD_LIST_END

    void listDeployments(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback);

    void startVerification(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback);

    void getVerification(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         const std::string& job_id);

    void sendMessage(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& job_id);

    void stopVerification(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& job_id);
};

} // namespace sfc
