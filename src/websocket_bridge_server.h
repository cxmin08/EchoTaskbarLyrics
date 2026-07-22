// SPDX-License-Identifier: GPL-3.0
// websocket_bridge_server.h - EchoMusic plugin bridge over local WebSocket
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <utility>

namespace ix {
class WebSocket;
class WebSocketServer;
} // namespace ix

namespace echo {

class WebSocketBridgeServer {
public:
    using CommandCallback = std::function<void(const std::string& command)>;
    using PayloadCallback = std::function<void(const nlohmann::json& payload)>;

    WebSocketBridgeServer();
    ~WebSocketBridgeServer();

    WebSocketBridgeServer(const WebSocketBridgeServer&) = delete;
    WebSocketBridgeServer& operator=(const WebSocketBridgeServer&) = delete;

    bool Start(int port);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    void OnCommand(CommandCallback cb) { onCommand_ = std::move(cb); }
    void OnLyrics(PayloadCallback cb) { onLyrics_ = std::move(cb); }
    // P0-3: 轻量心跳（仅播放状态，不含歌词），回调直接接收已解析的 data 对象。
    void OnHeartbeat(PayloadCallback cb) { onHeartbeat_ = std::move(cb); }

    bool SendControlCommand(const std::string& command);

private:
    bool IsAuthorizedUri(const std::string& uri) const;
    void ClearActiveClient(const std::string& connectionId);
    void SetActiveClient(const std::string& connectionId, std::weak_ptr<ix::WebSocket> webSocket);

    std::unique_ptr<ix::WebSocketServer> server_;
    std::atomic<bool> running_{false};
    CommandCallback onCommand_;
    PayloadCallback onLyrics_;
    PayloadCallback onHeartbeat_;

    mutable std::mutex clientMutex_;
    std::weak_ptr<ix::WebSocket> activeClient_;
    std::string activeClientId_;
};

} // namespace echo
