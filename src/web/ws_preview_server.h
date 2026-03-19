#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — WebSocket Preview Server
// Real-time preview streaming over WebSocket for unlimited concurrent streams
// Solves the browser's 6-connection HTTP/1.1 limit by multiplexing all
// preview streams over a single WebSocket connection.
// ─────────────────────────────────────────────────────────────────

#include "core/bot_manager.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <map>

namespace sm
{

    // Subscription info for a client
    struct PreviewSubscription
    {
        std::string username;
        std::string site;
        uint64_t lastVersion{0};
    };

    // Client connection state
    struct WsClient
    {
        ix::WebSocket *socket = nullptr;                          // Raw pointer - server owns lifetime
        std::map<std::string, PreviewSubscription> subscriptions; // key: "username:site"
        std::mutex mutex;
    };

    class WsPreviewServer
    {
    public:
        explicit WsPreviewServer(BotManager &manager, int port);
        ~WsPreviewServer();

        // Non-copyable
        WsPreviewServer(const WsPreviewServer &) = delete;
        WsPreviewServer &operator=(const WsPreviewServer &) = delete;

        // Start/stop the WebSocket server
        bool start();
        void stop();
        bool isRunning() const;

        int getPort() const { return port_; }

    private:
        void onMessage(std::shared_ptr<ix::ConnectionState> connectionState,
                       ix::WebSocket &webSocket,
                       const ix::WebSocketMessagePtr &msg);

        void broadcastLoop();
        std::string makeSubscriptionKey(const std::string &username, const std::string &site);

        BotManager &manager_;
        int port_;
        std::unique_ptr<ix::WebSocketServer> server_;
        std::unique_ptr<std::thread> broadcastThread_;
        std::atomic<bool> running_{false};

        // Connected clients
        std::mutex clientsMutex_;
        std::map<std::string, std::shared_ptr<WsClient>> clients_; // connectionId -> client
    };

} // namespace sm
