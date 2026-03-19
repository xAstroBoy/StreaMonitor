#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Embedded Web Server
// REST API + static file serving for Next.js dashboard
// HTTP/2 with TLS (single port, unlimited concurrent streams)
// Falls back to HTTP/1.1 for non-h2 clients (curl, etc.)
// ─────────────────────────────────────────────────────────────────

#include "core/bot_manager.h"
#include "config/config.h"
#include "web/h2_server.h"
#include <spdlog/sinks/ringbuffer_sink.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

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

        // Attach a ring buffer sink for /api/logs
        void setLogRingBuffer(std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink) { logRingBuffer_ = std::move(sink); }

    private:
        void setupRoutes();
        void setupStaticFiles();
        void setupCORS();
        std::string getLocalIP() const;

        // Authentication
        bool checkAuth(const H2Request &req, H2Response &res);
        std::string generateToken() const;
        bool validateToken(const std::string &token) const;
        mutable std::mutex tokenMutex_;
        mutable std::set<std::string> validTokens_;

        BotManager &manager_;
        AppConfig &config_;
        ModelConfigStore &configStore_;
        std::unique_ptr<H2Server> server_;
        std::atomic<bool> running_{false};
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> logRingBuffer_;
        std::string certPath_;
        std::string keyPath_;
    };

} // namespace sm
