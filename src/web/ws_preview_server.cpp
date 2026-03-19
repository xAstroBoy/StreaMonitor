// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — WebSocket Preview Server Implementation
// Real-time preview streaming over WebSocket
// ─────────────────────────────────────────────────────────────────

#include "web/ws_preview_server.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <chrono>

// stb_image_write — JPEG encoding for preview frames
#define STBIW_HEADER_FILE_ONLY
#include "stb_image_write.h"

using json = nlohmann::json;

namespace sm
{

    // JPEG encoding helper (same as web_server.cpp)
    namespace
    {
        void jpegWriteCallback(void *context, void *data, int size)
        {
            auto *out = static_cast<std::string *>(context);
            out->append(static_cast<const char *>(data), size);
        }

        std::string rgbaToJpeg(const uint8_t *rgba, int w, int h, int quality = 80)
        {
            std::string out;
            out.reserve(w * h / 4); // rough estimate for JPEG size

            // Convert RGBA to RGB (strip alpha)
            std::vector<uint8_t> rgb(w * h * 3);
            for (int i = 0; i < w * h; ++i)
            {
                rgb[i * 3 + 0] = rgba[i * 4 + 0];
                rgb[i * 3 + 1] = rgba[i * 4 + 1];
                rgb[i * 3 + 2] = rgba[i * 4 + 2];
            }

            stbi_write_jpg_to_func(jpegWriteCallback, &out, w, h, 3, rgb.data(), quality);
            return out;
        }
    }

    WsPreviewServer::WsPreviewServer(BotManager &manager, int port)
        : manager_(manager), port_(port)
    {
    }

    WsPreviewServer::~WsPreviewServer()
    {
        stop();
    }

    bool WsPreviewServer::start()
    {
        if (running_.load())
            return true;

        server_ = std::make_unique<ix::WebSocketServer>(port_, "0.0.0.0");

        server_->setOnClientMessageCallback(
            [this](std::shared_ptr<ix::ConnectionState> connectionState,
                   ix::WebSocket &webSocket,
                   const ix::WebSocketMessagePtr &msg)
            {
                onMessage(connectionState, webSocket, msg);
            });

        auto res = server_->listen();
        if (!res.first)
        {
            spdlog::error("WebSocket server failed to start on port {}: {}", port_, res.second);
            return false;
        }

        server_->start();
        running_.store(true);

        // Start broadcast loop thread
        broadcastThread_ = std::make_unique<std::thread>([this]()
                                                         { broadcastLoop(); });

        spdlog::info("WebSocket preview server started on port {}", port_);
        return true;
    }

    void WsPreviewServer::stop()
    {
        if (!running_.load())
            return;

        running_.store(false);

        if (broadcastThread_ && broadcastThread_->joinable())
            broadcastThread_->join();

        if (server_)
        {
            server_->stop();
            server_.reset();
        }

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.clear();
        }

        spdlog::info("WebSocket preview server stopped");
    }

    bool WsPreviewServer::isRunning() const
    {
        return running_.load();
    }

    std::string WsPreviewServer::makeSubscriptionKey(const std::string &username, const std::string &site)
    {
        return username + ":" + site;
    }

    void WsPreviewServer::onMessage(std::shared_ptr<ix::ConnectionState> connectionState,
                                    ix::WebSocket &webSocket,
                                    const ix::WebSocketMessagePtr &msg)
    {
        std::string connId = connectionState->getId();

        if (msg->type == ix::WebSocketMessageType::Open)
        {
            spdlog::debug("WS client connected: {}", connId);

            auto client = std::make_shared<WsClient>();
            client->socket = &webSocket; // Store raw pointer

            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_[connId] = client;
        }
        else if (msg->type == ix::WebSocketMessageType::Close)
        {
            spdlog::debug("WS client disconnected: {}", connId);

            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.erase(connId);
        }
        else if (msg->type == ix::WebSocketMessageType::Message)
        {
            // Handle subscribe/unsubscribe commands
            try
            {
                auto j = json::parse(msg->str);
                std::string cmd = j.value("cmd", "");
                std::string username = j.value("username", "");
                std::string site = j.value("site", "");

                if (username.empty() || site.empty())
                    return;

                std::string key = makeSubscriptionKey(username, site);

                std::lock_guard<std::mutex> lock(clientsMutex_);
                auto it = clients_.find(connId);
                if (it == clients_.end())
                    return;

                auto &client = it->second;
                std::lock_guard<std::mutex> clientLock(client->mutex);

                if (cmd == "subscribe")
                {
                    // Add subscription
                    PreviewSubscription sub;
                    sub.username = username;
                    sub.site = site;
                    sub.lastVersion = 0;
                    client->subscriptions[key] = sub;

                    spdlog::debug("WS client {} subscribed to {}:{}", connId, username, site);

                    // Send immediate ack
                    json ack = {
                        {"type", "subscribed"},
                        {"username", username},
                        {"site", site}};
                    webSocket.send(ack.dump());
                }
                else if (cmd == "unsubscribe")
                {
                    // Remove subscription
                    client->subscriptions.erase(key);
                    spdlog::debug("WS client {} unsubscribed from {}:{}", connId, username, site);
                }
            }
            catch (const std::exception &e)
            {
                spdlog::warn("WS message parse error: {}", e.what());
            }
        }
        else if (msg->type == ix::WebSocketMessageType::Error)
        {
            spdlog::warn("WS error for {}: {}", connId, msg->errorInfo.reason);
        }
    }

    void WsPreviewServer::broadcastLoop()
    {
        spdlog::debug("WS broadcast loop started");

        while (running_.load())
        {
            // Collect all unique subscriptions
            std::map<std::string, std::vector<std::pair<std::string, std::shared_ptr<WsClient>>>> subsByKey;

            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                for (auto &[connId, client] : clients_)
                {
                    std::lock_guard<std::mutex> clientLock(client->mutex);
                    for (auto &[key, sub] : client->subscriptions)
                    {
                        subsByKey[key].emplace_back(connId, client);
                    }
                }
            }

            // For each unique subscription, check for new frames
            for (auto &[key, clientList] : subsByKey)
            {
                // Parse key back to username:site
                auto colonPos = key.find(':');
                if (colonPos == std::string::npos)
                    continue;

                std::string username = key.substr(0, colonPos);
                std::string site = key.substr(colonPos + 1);

                // Get preview frame (non-blocking check)
                PreviewFrame frame;
                uint64_t reqVersion = 0;

                // Find minimum version needed (any client needs newer)
                {
                    std::lock_guard<std::mutex> lock(clientsMutex_);
                    for (auto &[connId, client] : clientList)
                    {
                        std::lock_guard<std::mutex> clientLock(client->mutex);
                        auto it = client->subscriptions.find(key);
                        if (it != client->subscriptions.end())
                        {
                            if (reqVersion == 0 || it->second.lastVersion < reqVersion)
                                reqVersion = it->second.lastVersion;
                        }
                    }
                }

                // Try to get a new frame (short timeout to avoid blocking other streams)
                if (!manager_.waitForPreview(username, site, frame, reqVersion, 50))
                    continue; // No new frame, check next subscription

                // Encode frame to JPEG
                std::string jpeg = rgbaToJpeg(frame.pixels.data(), frame.width, frame.height, 75);

                // Base64 encode for JSON transport (or use binary frames)
                // For efficiency, we send binary WebSocket frames with a small header

                // Header format: 1 byte type (0x01 = frame) + 2 bytes username len + username + 2 bytes site len + site + jpeg data
                std::string binaryMsg;
                binaryMsg.reserve(1 + 2 + username.size() + 2 + site.size() + jpeg.size());

                binaryMsg.push_back(0x01); // Frame type

                uint16_t ulen = static_cast<uint16_t>(username.size());
                binaryMsg.push_back(static_cast<char>(ulen >> 8));
                binaryMsg.push_back(static_cast<char>(ulen & 0xFF));
                binaryMsg.append(username);

                uint16_t slen = static_cast<uint16_t>(site.size());
                binaryMsg.push_back(static_cast<char>(slen >> 8));
                binaryMsg.push_back(static_cast<char>(slen & 0xFF));
                binaryMsg.append(site);

                binaryMsg.append(jpeg);

                // Broadcast to all clients subscribed to this stream
                {
                    std::lock_guard<std::mutex> lock(clientsMutex_);
                    for (auto &[connId, client] : clientList)
                    {
                        std::lock_guard<std::mutex> clientLock(client->mutex);
                        auto it = client->subscriptions.find(key);
                        if (it != client->subscriptions.end())
                        {
                            // Update with the version we actually received (reqVersion was min requested)
                            it->second.lastVersion = reqVersion;

                            if (client->socket && client->socket->getReadyState() == ix::ReadyState::Open)
                            {
                                client->socket->sendBinary(binaryMsg);
                            }
                        }
                    }
                }
            }

            // Small sleep to avoid busy-spinning when no frames are available
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps max check rate
        }

        spdlog::debug("WS broadcast loop stopped");
    }

} // namespace sm
