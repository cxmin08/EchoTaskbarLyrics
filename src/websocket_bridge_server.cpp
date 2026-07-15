// SPDX-License-Identifier: GPL-3.0
// websocket_bridge_server.cpp - EchoMusic plugin bridge over local WebSocket
#include "websocket_bridge_server.h"

#include "config.h"
#include "constants.h"
#include "logger.h"

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include <utility>

namespace echo {

using json = nlohmann::json;

namespace {

bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    int diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

std::string ExtractQueryParam(const std::string& uri, const std::string& key) {
    const auto queryPos = uri.find('?');
    if (queryPos == std::string::npos) return {};

    size_t pos = queryPos + 1;
    while (pos < uri.size()) {
        const auto nextAmp = uri.find('&', pos);
        const auto end = nextAmp == std::string::npos ? uri.size() : nextAmp;
        const auto eq = uri.find('=', pos);
        if (eq != std::string::npos && eq < end && uri.substr(pos, eq - pos) == key) {
            return uri.substr(eq + 1, end - eq - 1);
        }
        if (nextAmp == std::string::npos) break;
        pos = nextAmp + 1;
    }
    return {};
}

bool IsShutdownCommand(const json& j) {
    if (j.value("type", "") != "shutdown" && j.value("type", "") != "command") {
        return false;
    }
    if (j.value("command", "") == "shutdown") return true;
    if (j.contains("payload") && j["payload"].is_object()) {
        const auto& payload = j["payload"];
        if (payload.value("action", "") == "shutdown") return true;
        if (payload.value("command", "") == "shutdown") return true;
    }
    return false;
}

} // namespace

WebSocketBridgeServer::WebSocketBridgeServer() = default;

WebSocketBridgeServer::~WebSocketBridgeServer() {
    Stop();
}

bool WebSocketBridgeServer::Start(int port) {
    if (running_.load()) return true;
    if (Config::IsUsingFallbackToken()) {
        Log("[BRIDGE] Refusing to start: fallback token active (auth unsafe)\n");
        return false;
    }

    server_ = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1");
    server_->disablePerMessageDeflate();

    server_->setOnConnectionCallback(
        [this](std::weak_ptr<ix::WebSocket> webSocket,
               std::shared_ptr<ix::ConnectionState> connectionState) {
            auto ws = webSocket.lock();
            if (!ws) return;

            ws->setOnMessageCallback(
                [this, webSocket, connectionState](const ix::WebSocketMessagePtr& msg) {
                    const std::string connectionId = connectionState ? connectionState->getId() : "";

                    if (msg->type == ix::WebSocketMessageType::Open) {
                        if (!IsAuthorizedUri(msg->openInfo.uri)) {
                            Log("[BRIDGE] Rejected unauthorized WebSocket client\n");
                            if (auto ws = webSocket.lock()) {
                                ws->close(4001, "Permission denied");
                            }
                            return;
                        }
                        SetActiveClient(connectionId, webSocket);
                        Log("[BRIDGE] WebSocket client connected\n");
                        return;
                    }

                    if (msg->type == ix::WebSocketMessageType::Close ||
                        msg->type == ix::WebSocketMessageType::Error) {
                        ClearActiveClient(connectionId);
                        Log("[BRIDGE] WebSocket client disconnected\n");
                        return;
                    }

                    if (msg->type != ix::WebSocketMessageType::Message) return;
                    if (msg->str.size() > constants::MAX_WS_MESSAGE_SIZE) {
                        Log("[BRIDGE] Dropped oversized message: %zu bytes\n", msg->str.size());
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> lock(clientMutex_);
                        if (connectionId.empty() || connectionId != activeClientId_) {
                            return;
                        }
                    }

                    try {
                        const auto j = json::parse(msg->str);
                        const std::string type = j.value("type", "");
                        if (type == "lyrics") {
                            if (onLyrics_) onLyrics_(j.value("data", json::object()).dump());
                        } else if (IsShutdownCommand(j)) {
                            if (onCommand_) onCommand_("shutdown");
                        } else if (type == "ping") {
                            if (auto ws = webSocket.lock()) {
                                ws->send(json{{"type", "pong"}}.dump());
                            }
                        }
                    } catch (const std::exception& e) {
                        Log("[BRIDGE] Failed to handle message: %s\n", e.what());
                    } catch (...) {
                        Log("[BRIDGE] Failed to handle message: unknown error\n");
                    }
                });
        });

    const auto listenResult = server_->listen();
    if (!listenResult.first) {
        Log("[BRIDGE] listen failed on port %d: %s\n", port, listenResult.second.c_str());
        server_.reset();
        return false;
    }

    server_->start();
    running_.store(true);
    Log("[BRIDGE] WebSocket server started on port %d\n", port);
    return true;
}

void WebSocketBridgeServer::Stop() {
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        activeClient_.reset();
        activeClientId_.clear();
    }
    if (server_) {
        try {
            server_->stop();
        } catch (...) {
        }
        server_.reset();
    }
}

bool WebSocketBridgeServer::SendControlCommand(const std::string& command) {
    if (command.empty()) return false;

    std::shared_ptr<ix::WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        ws = activeClient_.lock();
    }
    if (!ws) return false;

    json j;
    j["type"] = "command";
    j["source"] = "native";
    j["payload"] = {{"action", command}, {"data", json::object()}};

    const auto result = ws->send(j.dump());
    return result.success;
}

bool WebSocketBridgeServer::IsAuthorizedUri(const std::string& uri) const {
    const std::string token = ExtractQueryParam(uri, "token");
    return !token.empty() && ConstantTimeEquals(token, Config::GetAuthToken());
}

void WebSocketBridgeServer::ClearActiveClient(const std::string& connectionId) {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (connectionId.empty() || connectionId == activeClientId_) {
        activeClient_.reset();
        activeClientId_.clear();
    }
}

void WebSocketBridgeServer::SetActiveClient(
    const std::string& connectionId,
    std::weak_ptr<ix::WebSocket> webSocket) {
    std::lock_guard<std::mutex> lock(clientMutex_);
    activeClientId_ = connectionId;
    activeClient_ = std::move(webSocket);
}

} // namespace echo
