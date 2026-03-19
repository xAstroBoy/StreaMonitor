#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Embedded Web Server
// REST API + static file serving for Next.js dashboard
// WebSocket server for real-time preview streaming (unlimited concurrent streams)
// Listens on all interfaces for WiFi intranet access
// ─────────────────────────────────────────────────────────────────

#include "core/bot_manager.h"
#include "config/config.h"
#include "web/ws_preview_server.h"
#include <spdlog/sinks/ringbuffer_sink.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

// Forward declare httplib types
namespace httplib
{
    class Server;
}

namespace sm
{

    class WebServer
    {
    public:
        explicit WebServer(BotManager &manager, AppConfig &config,
                           ModelConfigStore &configStore);
        ~WebServer();

        // Non-copyable
        WebServer(const WebServer &) = delete;
        WebServer &operator=(const WebServer &) = delete;

        // Start/stop the server
        bool start();
        void stop();
        bool isRunning() const;

        // Get the URL the server is listening on
        std::string getUrl() const;
        std::string getLocalUrl() const;
        std::string getNetworkUrl() const;

        // WebSocket port for preview streaming
        int getWsPort() const;

        // Attach a ring buffer sink for /api/logs
        void setLogRingBuffer(std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink) { logRingBuffer_ = std::move(sink); }

    private:
        void setupRoutes();
        void setupStaticFiles();
        void setupCORS();
        std::string getLocalIP() const;

        // Authentication
        bool checkAuth(const void *req, void *res);
        std::string generateToken() const;
        bool validateToken(const std::string &token) const;
        mutable std::mutex tokenMutex_;
        mutable std::set<std::string> validTokens_;

        // API route handlers
        void apiGetStatus(void *req, void *res);
        void apiGetModels(void *req, void *res);
        void apiAddModel(void *req, void *res);
        void apiRemoveModel(void *req, void *res);
        void apiStartModel(void *req, void *res);
        void apiStopModel(void *req, void *res);
        void apiRestartModel(void *req, void *res);
        void apiGetSites(void *req, void *res);
        void apiGetGroups(void *req, void *res);
        void apiCreateGroup(void *req, void *res);
        void apiDeleteGroup(void *req, void *res);
        void apiGetConfig(void *req, void *res);
        void apiUpdateConfig(void *req, void *res);
        void apiGetDiskUsage(void *req, void *res);
        void apiSaveConfig(void *req, void *res);
        void apiStartAll(void *req, void *res);
        void apiStopAll(void *req, void *res);

        BotManager &manager_;
        AppConfig &config_;
        ModelConfigStore &configStore_;
        std::unique_ptr<httplib::Server> server_;
        std::unique_ptr<std::thread> serverThread_;
        std::unique_ptr<WsPreviewServer> wsServer_;
        std::atomic<bool> running_{false};
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> logRingBuffer_;
    };

} // namespace sm
