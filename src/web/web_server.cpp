// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Embedded Web Server Implementation
// REST API + static file serving for the Next.js dashboard
// ─────────────────────────────────────────────────────────────────

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "web/web_server.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <random>
#include <iomanip>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <ifaddrs.h>
#include <arpa/inet.h>
#endif

using json = nlohmann::json;

namespace sm
{

    WebServer::WebServer(BotManager &manager, AppConfig &config,
                         ModelConfigStore &configStore)
        : manager_(manager), config_(config), configStore_(configStore),
          server_(std::make_unique<httplib::Server>())
    {
    }

    WebServer::~WebServer()
    {
        stop();
    }

    bool WebServer::start()
    {
        if (running_.load())
            return true;

        setupCORS();
        setupRoutes();
        setupStaticFiles();

        serverThread_ = std::make_unique<std::thread>([this]()
                                                      {
            running_.store(true);
            spdlog::info("Web server starting on {}:{}", config_.webHost, config_.webPort);
            spdlog::info("  Local:   {}", getLocalUrl());
            spdlog::info("  Network: {}", getNetworkUrl());

            if (!server_->listen(config_.webHost, config_.webPort)) {
                spdlog::error("Web server failed to start on {}:{}", config_.webHost, config_.webPort);
                running_.store(false);
            } });

        // Give server a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return running_.load();
    }

    void WebServer::stop()
    {
        if (!running_.load())
            return;
        server_->stop();
        if (serverThread_ && serverThread_->joinable())
            serverThread_->join();
        running_.store(false);
        spdlog::info("Web server stopped");
    }

    bool WebServer::isRunning() const
    {
        return running_.load();
    }

    std::string WebServer::getUrl() const
    {
        return "http://" + config_.webHost + ":" + std::to_string(config_.webPort);
    }

    std::string WebServer::getLocalUrl() const
    {
        return "http://127.0.0.1:" + std::to_string(config_.webPort);
    }

    std::string WebServer::getNetworkUrl() const
    {
        std::string ip = getLocalIP();
        if (ip.empty())
            ip = "127.0.0.1";
        return "http://" + ip + ":" + std::to_string(config_.webPort);
    }

    // ─────────────────────────────────────────────────────────────────
    // CORS setup (allow access from any origin for dev/LAN)
    // ─────────────────────────────────────────────────────────────────
    void WebServer::setupCORS()
    {
        server_->set_default_headers({{"Access-Control-Allow-Origin", "*"},
                                      {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
                                      {"Access-Control-Allow-Headers", "Content-Type, Authorization"}});

        // Handle preflight OPTIONS requests
        server_->Options(".*", [](const httplib::Request &, httplib::Response &res)
                         { res.status = 204; });
    }

    // ─────────────────────────────────────────────────────────────────
    // Static file serving (Next.js export output)
    // ─────────────────────────────────────────────────────────────────
    void WebServer::setupStaticFiles()
    {
        std::filesystem::path staticDir = config_.webStaticDir;
        if (!staticDir.is_absolute())
        {
            // Resolve relative to executable directory
            auto exePath = std::filesystem::current_path();
            staticDir = exePath / staticDir;
        }

        if (std::filesystem::exists(staticDir))
        {
            server_->set_mount_point("/", staticDir.string());
            spdlog::info("Serving static files from: {}", staticDir.string());

            // SPA fallback: serve index.html for non-API, non-file routes
            server_->set_error_handler([staticDir](const httplib::Request &req, httplib::Response &res)
                                       {
                if (res.status == 404 && req.path.substr(0, 4) != "/api") {
                    auto indexPath = staticDir / "index.html";
                    if (std::filesystem::exists(indexPath)) {
                        std::ifstream ifs(indexPath.string());
                        if (ifs.is_open()) {
                            std::string content((std::istreambuf_iterator<char>(ifs)),
                                               std::istreambuf_iterator<char>());
                            res.set_content(content, "text/html");
                            res.status = 200;
                        }
                    }
                } });
        }
        else
        {
            spdlog::warn("Static web directory not found: {}. Dashboard unavailable.", staticDir.string());

            // Serve a simple fallback page
            server_->Get("/", [](const httplib::Request &, httplib::Response &res)
                         { res.set_content(R"html(<!DOCTYPE html>
<html><head><title>StreaMonitor</title></head>
<body style="background:#0a0a0a;color:#fff;font-family:system-ui;display:flex;align-items:center;justify-content:center;height:100vh;margin:0">
<div style="text-align:center">
<h1>🎬 StreaMonitor v2.0</h1>
<p>API is running. Dashboard static files not found.</p>
<p>Place the Next.js export in the <code>web/</code> directory.</p>
<p><a href="/api/status" style="color:#60a5fa">API Status →</a></p>
</div></body></html>)html",
                                           "text/html"); });
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Helper: RGBA → BMP conversion (24-bit, browser-compatible)
    // ─────────────────────────────────────────────────────────────────
    static std::string rgbaToBmp(const uint8_t *rgba, int w, int h)
    {
        int rowSize = ((w * 3 + 3) / 4) * 4; // rows padded to 4-byte boundary
        int imageSize = rowSize * h;
        int fileSize = 54 + imageSize;

        std::string bmp(fileSize, '\0');
        auto *p = reinterpret_cast<uint8_t *>(&bmp[0]);

        // BMP file header (14 bytes)
        p[0] = 'B';
        p[1] = 'M';
        std::memcpy(p + 2, &fileSize, 4);
        int offset = 54;
        std::memcpy(p + 10, &offset, 4);

        // DIB header (BITMAPINFOHEADER, 40 bytes)
        int dibSize = 40;
        std::memcpy(p + 14, &dibSize, 4);
        std::memcpy(p + 18, &w, 4);
        std::memcpy(p + 22, &h, 4); // positive = bottom-up
        uint16_t planes = 1, bpp = 24;
        std::memcpy(p + 26, &planes, 2);
        std::memcpy(p + 28, &bpp, 2);
        std::memcpy(p + 34, &imageSize, 4);

        // Pixel data: BGR, bottom-up scanlines
        for (int y = 0; y < h; y++)
        {
            const uint8_t *src = rgba + (h - 1 - y) * w * 4; // flip vertically
            uint8_t *dst = p + 54 + y * rowSize;
            for (int x = 0; x < w; x++)
            {
                dst[x * 3 + 0] = src[x * 4 + 2]; // B
                dst[x * 3 + 1] = src[x * 4 + 1]; // G
                dst[x * 3 + 2] = src[x * 4 + 0]; // R
            }
        }

        return bmp;
    }

    // ─────────────────────────────────────────────────────────────────
    // Helper: JSON response
    // ─────────────────────────────────────────────────────────────────
    static void jsonResponse(httplib::Response &res, const json &j, int status = 200)
    {
        res.set_content(j.dump(), "application/json");
        res.status = status;
    }

    static void jsonError(httplib::Response &res, const std::string &msg, int status = 400)
    {
        json j = {{"error", msg}};
        res.set_content(j.dump(), "application/json");
        res.status = status;
    }

    // ─────────────────────────────────────────────────────────────────
    // Authentication helpers
    // ─────────────────────────────────────────────────────────────────
    std::string WebServer::generateToken() const
    {
        static const char alphanum[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

        std::string token;
        token.reserve(64);
        for (int i = 0; i < 64; ++i)
            token += alphanum[dis(gen)];
        return token;
    }

    bool WebServer::validateToken(const std::string &token) const
    {
        std::lock_guard lock(tokenMutex_);
        return validTokens_.find(token) != validTokens_.end();
    }

    // Returns true if authenticated, false if response was sent with 401
    bool WebServer::checkAuth(const void *reqPtr, void *resPtr)
    {
        const auto &req = *static_cast<const httplib::Request *>(reqPtr);
        auto &res = *static_cast<httplib::Response *>(resPtr);

        // Check for Authorization header: "Bearer <token>"
        auto authHeader = req.get_header_value("Authorization");
        if (authHeader.size() > 7 && authHeader.substr(0, 7) == "Bearer ")
        {
            std::string token = authHeader.substr(7);
            if (validateToken(token))
                return true;
        }

        // Check for token in cookie
        auto cookieHeader = req.get_header_value("Cookie");
        if (!cookieHeader.empty())
        {
            // Parse cookies: "token=abc123; other=..."
            size_t pos = cookieHeader.find("sm_token=");
            if (pos != std::string::npos)
            {
                size_t start = pos + 9;
                size_t end = cookieHeader.find(';', start);
                std::string token = cookieHeader.substr(start, end == std::string::npos ? end : end - start);
                if (validateToken(token))
                    return true;
            }
        }

        jsonError(res, "Authentication required", 401);
        return false;
    }

    // Helper: serialize BotState to JSON
    static json botStateToJson(const BotState &st)
    {
        auto now = Clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - st.startTime).count();
        auto sinceChange = std::chrono::duration_cast<std::chrono::seconds>(now - st.lastStatusChange).count();

        return {
            {"username", st.username},
            {"site", st.siteName},
            {"siteSlug", st.siteSlug},
            {"status", statusToString(st.status)},
            {"statusCode", static_cast<int>(st.status)},
            {"prevStatus", statusToString(st.prevStatus)},
            {"running", st.running},
            {"recording", st.recording},
            {"quitting", st.quitting},
            {"mobile", st.mobile},
            {"gender", genderToString(st.gender)},
            {"country", st.country},
            {"websiteUrl", st.websiteUrl},
            {"previewUrl", st.previewUrl},
            {"groupName", st.groupName},
            {"roomId", st.roomId},
            {"consecutiveErrors", st.consecutiveErrors},
            {"fileCount", st.fileCount},
            {"totalBytes", st.totalBytes},
            {"currentFile", st.currentFile},
            {"uptimeSeconds", uptime},
            {"timeSinceStatusChange", sinceChange},
            {"recording_stats", {{"bytesWritten", st.recordingStats.bytesWritten}, {"segmentsRecorded", st.recordingStats.segmentsRecorded}, {"stallsDetected", st.recordingStats.stallsDetected}, {"restartsPerformed", st.recordingStats.restartsPerformed}, {"currentSpeed", st.recordingStats.currentSpeed}, {"currentFile", st.recordingStats.currentFile}}}};
    }

    // ─────────────────────────────────────────────────────────────────
    // API Routes
    // ─────────────────────────────────────────────────────────────────
    void WebServer::setupRoutes()
    {
        // ── POST /api/auth/login — Authenticate and get token ─────
        server_->Post("/api/auth/login", [this](const httplib::Request &req, httplib::Response &res)
                      {
            try {
                auto body = json::parse(req.body);
                std::string username = body.value("username", "");
                std::string password = body.value("password", "");

                if (username == config_.webUsername && password == config_.webPassword) {
                    std::string token = generateToken();
                    {
                        std::lock_guard lock(tokenMutex_);
                        validTokens_.insert(token);
                    }
                    // Set cookie with token (HttpOnly for security)
                    res.set_header("Set-Cookie", "sm_token=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=86400");
                    json j = {{"success", true}, {"token", token}};
                    jsonResponse(res, j);
                    spdlog::info("Web login successful for user: {}", username);
                } else {
                    jsonError(res, "Invalid credentials", 401);
                    spdlog::warn("Web login failed for user: {}", username);
                }
            } catch (const std::exception &e) {
                jsonError(res, "Invalid request body", 400);
            } });

        // ── POST /api/auth/logout — Invalidate token ──────────────
        server_->Post("/api/auth/logout", [this](const httplib::Request &req, httplib::Response &res)
                      {
            // Extract token and remove it
            auto authHeader = req.get_header_value("Authorization");
            if (authHeader.size() > 7 && authHeader.substr(0, 7) == "Bearer ") {
                std::string token = authHeader.substr(7);
                std::lock_guard lock(tokenMutex_);
                validTokens_.erase(token);
            }
            // Also check cookie
            auto cookieHeader = req.get_header_value("Cookie");
            if (!cookieHeader.empty()) {
                size_t pos = cookieHeader.find("sm_token=");
                if (pos != std::string::npos) {
                    size_t start = pos + 9;
                    size_t end = cookieHeader.find(';', start);
                    std::string token = cookieHeader.substr(start, end == std::string::npos ? end : end - start);
                    std::lock_guard lock(tokenMutex_);
                    validTokens_.erase(token);
                }
            }
            // Clear cookie
            res.set_header("Set-Cookie", "sm_token=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
            json j = {{"success", true}};
            jsonResponse(res, j); });

        // ── GET /api/auth/check — Validate current session ────────
        server_->Get("/api/auth/check", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (checkAuth(&req, &res)) {
                json j = {{"authenticated", true}, {"username", config_.webUsername}};
                jsonResponse(res, j);
            } });

        // ── PUT /api/auth/credentials — Update login credentials ──
        server_->Put("/api/auth/credentials", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (!checkAuth(&req, &res)) return;
            try {
                auto body = json::parse(req.body);
                std::string newUser = body.value("username", "");
                std::string newPass = body.value("password", "");
                std::string currentPass = body.value("currentPassword", "");

                // Verify current password
                if (currentPass != config_.webPassword) {
                    jsonError(res, "Current password incorrect", 403);
                    return;
                }

                if (!newUser.empty()) config_.webUsername = newUser;
                if (!newPass.empty()) config_.webPassword = newPass;

                // Invalidate all tokens (force re-login)
                {
                    std::lock_guard lock(tokenMutex_);
                    validTokens_.clear();
                }

                json j = {{"success", true}, {"message", "Credentials updated. Please log in again."}};
                jsonResponse(res, j);
                spdlog::info("Web credentials updated. Username: {}", config_.webUsername);
            } catch (const std::exception &e) {
                jsonError(res, "Invalid request body", 400);
            } });

        // ── GET /api/status — Server overview ─────────────────────
        server_->Get("/api/status", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (!checkAuth(&req, &res)) return;
            auto states = manager_.getAllStates();
            size_t online = 0, recording = 0;
            for (const auto &s : states) {
                if (s.status == Status::Public || s.status == Status::Online) online++;
                if (s.recording) recording++;
            }
            auto disk = manager_.getDiskUsage();
            json j = {
                {"version", "2.0.0"},
                {"totalModels", states.size()},
                {"onlineModels", online},
                {"recordingModels", recording},
                {"disk", {
                    {"totalBytes", disk.totalBytes},
                    {"freeBytes", disk.freeBytes},
                    {"downloadDirBytes", disk.downloadDirBytes},
                    {"fileCount", disk.fileCount}
                }}
            };
            jsonResponse(res, j); });

        // ── GET /api/models — All models with states ──────────────
        server_->Get("/api/models", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (!checkAuth(&req, &res)) return;
            auto states = manager_.getAllStates();
            json arr = json::array();
            for (const auto &st : states) {
                arr.push_back(botStateToJson(st));
            }
            jsonResponse(res, arr); });

        // ── GET /api/preview/:username/:site — Serve preview PNG ──
        // Requests an on-demand preview frame from the live recorder.
        // The recorder decodes the next keyframe to RGBA; we wait
        // briefly and return the raw RGBA as a PNG (via stb_image_write
        // in memory). If no frame arrives in time → 404.
        server_->Get(R"(/api/preview/([^/]+)/([^/]+))",
                     [this](const httplib::Request &req, httplib::Response &res)
                     {
                         std::string username = req.matches[1].str();
                         std::string site = req.matches[2].str();

                         // Request a preview and poll for up to 3 seconds
                         manager_.requestPreview(username, site);

                         PreviewFrame frame;
                         for (int i = 0; i < 30; ++i)
                         {
                             if (manager_.consumePreview(username, site, frame))
                                 break;
                             std::this_thread::sleep_for(std::chrono::milliseconds(100));
                         }

                         if (frame.empty())
                         {
                             res.status = 404;
                             res.set_content("No preview available", "text/plain");
                             return;
                         }

                         // Convert RGBA to BMP for browser-compatible display
                         std::string bmp = rgbaToBmp(frame.pixels.data(), frame.width, frame.height);
                         res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
                         res.set_content(bmp, "image/bmp");
                     });

        // ── POST /api/models — Add a model ────────────────────────
        server_->Post("/api/models", [this](const httplib::Request &req, httplib::Response &res)
                      {
            if (!checkAuth(&req, &res)) return;
            try {
                auto body = json::parse(req.body);
                std::string username = body.value("username", "");
                std::string site = body.value("site", "");
                bool autoStart = body.value("autoStart", true);

                if (username.empty() || site.empty()) {
                    jsonError(res, "username and site are required");
                    return;
                }

                // SC* alias: add both SC (StripChat) and SCVR (StripChatVR)
                std::string siteLower = site;
                std::transform(siteLower.begin(), siteLower.end(), siteLower.begin(), ::tolower);
                std::vector<std::string> sitesToAdd;
                if (siteLower == "sc*")
                    sitesToAdd = {"SC", "SCVR"};
                else
                    sitesToAdd = {site};

                bool anyAdded = false;
                for (const auto &s : sitesToAdd) {
                    if (manager_.addBot(username, s, autoStart))
                        anyAdded = true;
                }

                if (anyAdded) {
                    jsonResponse(res, {{"success", true}, {"message", "Model added"}}, 201);
                } else {
                    jsonError(res, "Failed to add model (already exists or invalid site)");
                }
            } catch (const std::exception &e) {
                jsonError(res, std::string("Invalid JSON: ") + e.what());
            } });

        // ── DELETE /api/models/:username/:site — Remove a model ───
        server_->Delete(R"(/api/models/([^/]+)/([^/]+))",
                        [this](const httplib::Request &req, httplib::Response &res)
                        {
                            if (!checkAuth(&req, &res))
                                return;
                            std::string username = req.matches[1].str();
                            std::string site = req.matches[2].str();

                            if (manager_.removeBot(username, site))
                            {
                                jsonResponse(res, {{"success", true}, {"message", "Model removed"}});
                            }
                            else
                            {
                                jsonError(res, "Model not found", 404);
                            }
                        });

        // ── POST /api/models/:username/:site/start ────────────────
        server_->Post(R"(/api/models/([^/]+)/([^/]+)/start)",
                      [this](const httplib::Request &req, httplib::Response &res)
                      {
                          if (!checkAuth(&req, &res))
                              return;
                          std::string username = req.matches[1].str();
                          std::string site = req.matches[2].str();

                          if (manager_.startBot(username, site))
                          {
                              jsonResponse(res, {{"success", true}});
                          }
                          else
                          {
                              jsonError(res, "Model not found", 404);
                          }
                      });

        // ── POST /api/models/:username/:site/stop ─────────────────
        server_->Post(R"(/api/models/([^/]+)/([^/]+)/stop)",
                      [this](const httplib::Request &req, httplib::Response &res)
                      {
                          if (!checkAuth(&req, &res))
                              return;
                          std::string username = req.matches[1].str();
                          std::string site = req.matches[2].str();

                          if (manager_.stopBot(username, site))
                          {
                              jsonResponse(res, {{"success", true}});
                          }
                          else
                          {
                              jsonError(res, "Model not found", 404);
                          }
                      });

        // ── POST /api/models/:username/:site/restart ──────────────
        server_->Post(R"(/api/models/([^/]+)/([^/]+)/restart)",
                      [this](const httplib::Request &req, httplib::Response &res)
                      {
                          if (!checkAuth(&req, &res))
                              return;
                          std::string username = req.matches[1].str();
                          std::string site = req.matches[2].str();

                          if (manager_.restartBot(username, site))
                          {
                              jsonResponse(res, {{"success", true}});
                          }
                          else
                          {
                              jsonError(res, "Model not found", 404);
                          }
                      });

        // ── POST /api/start-all ───────────────────────────────────
        server_->Post("/api/start-all", [this](const httplib::Request &req, httplib::Response &res)
                      {
            if (!checkAuth(&req, &res)) return;
            manager_.startAllBots();
            manager_.startAllGroups();
            jsonResponse(res, {{"success", true}, {"message", "All bots and groups started"}}); });

        // ── POST /api/stop-all ────────────────────────────────────
        server_->Post("/api/stop-all", [this](const httplib::Request &req, httplib::Response &res)
                      {
            if (!checkAuth(&req, &res)) return;
            manager_.stopAllGroups();
            manager_.stopAll();
            jsonResponse(res, {{"success", true}, {"message", "All bots and groups stopped"}}); });

        // ── GET /api/sites — Available sites ──────────────────────
        server_->Get("/api/sites", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (!checkAuth(&req, &res)) return;
            auto sites = manager_.availableSites();
            auto &registry = SiteRegistry::instance();
            json arr = json::array();
            for (const auto &name : sites) {
                arr.push_back({
                    {"name", name},
                    {"slug", registry.nameToSlug(name)}
                });
            }
            jsonResponse(res, arr); });

        // ── GET /api/groups — Cross-register groups with states ────
        server_->Get("/api/groups", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (!checkAuth(&req, &res)) return;
            auto groups = manager_.getCrossRegisterGroups();
            auto groupStates = manager_.getAllGroupStates();
            json arr = json::array();
            for (const auto &g : groups) {
                json members = json::array();
                for (const auto &[site, user] : g.members) {
                    members.push_back({{"site", site}, {"username", user}});
                }
                // Find matching group state
                json stateJ = {{"running", false}, {"recording", false},
                               {"activeUsername", ""}, {"activeSite", ""},
                               {"activeStatus", "NotRunning"}};
                for (const auto &gs : groupStates) {
                    if (gs.groupName == g.groupName) {
                        json pairings = json::array();
                        for (const auto &ps : gs.pairings) {
                            pairings.push_back({
                                {"site", ps.site}, {"username", ps.username},
                                {"status", statusToString(ps.lastStatus)},
                                {"mobile", ps.mobile}
                            });
                        }
                        stateJ = {
                            {"running", gs.running},
                            {"recording", gs.recording},
                            {"activePairingIdx", gs.activePairingIdx},
                            {"activeUsername", gs.activeUsername},
                            {"activeSite", gs.activeSite},
                            {"activeStatus", statusToString(gs.activeStatus)},
                            {"activeMobile", gs.activeMobile},
                            {"pairings", pairings}
                        };
                        break;
                    }
                }
                arr.push_back({
                    {"name", g.groupName},
                    {"members", members},
                    {"linkRecording", g.linkRecording},
                    {"linkStatus", g.linkStatus},
                    {"state", stateJ}
                });
            }
            jsonResponse(res, arr); });

        // ── POST /api/groups — Create a group ─────────────────────
        server_->Post("/api/groups", [this](const httplib::Request &req, httplib::Response &res)
                      {            if (!checkAuth(&req, &res)) return;            try {
                auto body = json::parse(req.body);
                std::string name = body.value("name", "");
                if (name.empty()) {
                    jsonError(res, "Group name required");
                    return;
                }

                std::vector<std::pair<std::string, std::string>> members;
                if (body.contains("members") && body["members"].is_array()) {
                    for (const auto &m : body["members"]) {
                        members.emplace_back(m.value("site", ""), m.value("username", ""));
                    }
                }

                if (manager_.createCrossRegisterGroup(name, members)) {
                    jsonResponse(res, {{"success", true}}, 201);
                } else {
                    jsonError(res, "Failed to create group");
                }
            } catch (const std::exception &e) {
                jsonError(res, std::string("Invalid JSON: ") + e.what());
            } });

        // ── DELETE /api/groups/:name ───────────────────────────────
        server_->Delete(R"(/api/groups/([^/]+))",
                        [this](const httplib::Request &req, httplib::Response &res)
                        {
                            if (!checkAuth(&req, &res))
                                return;
                            std::string name = req.matches[1].str();
                            if (manager_.removeCrossRegisterGroup(name))
                            {
                                jsonResponse(res, {{"success", true}});
                            }
                            else
                            {
                                jsonError(res, "Group not found", 404);
                            }
                        });

        // ── POST /api/groups/:name/start ──────────────────────────
        server_->Post(R"(/api/groups/([^/]+)/start)",
                      [this](const httplib::Request &req, httplib::Response &res)
                      {
                          if (!checkAuth(&req, &res))
                              return;
                          std::string name = req.matches[1].str();
                          if (manager_.startGroup(name))
                              jsonResponse(res, {{"success", true}});
                          else
                              jsonError(res, "Group not found or already running", 404);
                      });

        // ── POST /api/groups/:name/stop ───────────────────────────
        server_->Post(R"(/api/groups/([^/]+)/stop)",
                      [this](const httplib::Request &req, httplib::Response &res)
                      {
                          if (!checkAuth(&req, &res))
                              return;
                          std::string name = req.matches[1].str();
                          if (manager_.stopGroup(name))
                              jsonResponse(res, {{"success", true}});
                          else
                              jsonError(res, "Group not found or not running", 404);
                      });

        // ── POST /api/groups/:name/members — Add member ───────────
        server_->Post(R"(/api/groups/([^/]+)/members)",
                      [this](const httplib::Request &req, httplib::Response &res)
                      {
                          if (!checkAuth(&req, &res))
                              return;
                          std::string name = req.matches[1].str();
                          try
                          {
                              auto body = json::parse(req.body);
                              std::string site = body.value("site", "");
                              std::string username = body.value("username", "");
                              if (site.empty() || username.empty())
                              {
                                  jsonError(res, "site and username required");
                                  return;
                              }
                              if (manager_.addToCrossRegisterGroup(name, site, username))
                                  jsonResponse(res, {{"success", true}});
                              else
                                  jsonError(res, "Failed to add member");
                          }
                          catch (const std::exception &e)
                          {
                              jsonError(res, std::string("Invalid JSON: ") + e.what());
                          }
                      });

        // ── DELETE /api/groups/:name/members — Remove member ──────
        server_->Delete(R"(/api/groups/([^/]+)/members)",
                        [this](const httplib::Request &req, httplib::Response &res)
                        {
                            if (!checkAuth(&req, &res))
                                return;
                            std::string name = req.matches[1].str();
                            try
                            {
                                auto body = json::parse(req.body);
                                std::string site = body.value("site", "");
                                std::string username = body.value("username", "");
                                if (site.empty() || username.empty())
                                {
                                    jsonError(res, "site and username required");
                                    return;
                                }
                                if (manager_.removeFromCrossRegisterGroup(name, site, username))
                                    jsonResponse(res, {{"success", true}});
                                else
                                    jsonError(res, "Failed to remove member");
                            }
                            catch (const std::exception &e)
                            {
                                jsonError(res, std::string("Invalid JSON: ") + e.what());
                            }
                        });

        // ── GET /api/config — App config ──────────────────────────
        server_->Get("/api/config", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (!checkAuth(&req, &res)) return;
            json j = {
                {"downloadsDir", config_.downloadsDir.string()},
                {"container", config_.container == ContainerFormat::MKV ? "mkv" :
                              config_.container == ContainerFormat::MP4 ? "mp4" : "ts"},
                {"wantedResolution", config_.wantedResolution},
                {"webEnabled", config_.webEnabled},
                {"webHost", config_.webHost},
                {"webPort", config_.webPort},
                {"debug", config_.debug},
                {"minFreeDiskPercent", config_.minFreeDiskPercent},
                {"httpTimeoutSec", config_.httpTimeoutSec},
                {"userAgent", config_.userAgent}
            };
            jsonResponse(res, j); });

        // ── PUT /api/config — Update config ───────────────────────
        server_->Put("/api/config", [this](const httplib::Request &req, httplib::Response &res)
                     {            if (!checkAuth(&req, &res)) return;            try {
                auto body = json::parse(req.body);

                if (body.contains("downloadsDir"))
                    config_.downloadsDir = body["downloadsDir"].get<std::string>();
                if (body.contains("container"))
                    config_.container = parseContainerFormat(body["container"].get<std::string>());
                if (body.contains("wantedResolution"))
                    config_.wantedResolution = body["wantedResolution"].get<int>();
                if (body.contains("debug"))
                    config_.debug = body["debug"].get<bool>();
                if (body.contains("minFreeDiskPercent"))
                    config_.minFreeDiskPercent = body["minFreeDiskPercent"].get<float>();

                jsonResponse(res, {{"success", true}, {"message", "Config updated"}});
            } catch (const std::exception &e) {
                jsonError(res, std::string("Invalid JSON: ") + e.what());
            } });

        // ── GET /api/disk — Disk usage ────────────────────────────
        server_->Get("/api/disk", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (!checkAuth(&req, &res)) return;
            auto info = manager_.getDiskUsage();
            json j = {
                {"totalBytes", info.totalBytes},
                {"freeBytes", info.freeBytes},
                {"downloadDirBytes", info.downloadDirBytes},
                {"fileCount", info.fileCount},
                {"usedPercent", info.totalBytes > 0 ?
                    100.0 * (1.0 - (double)info.freeBytes / info.totalBytes) : 0.0}
            };
            jsonResponse(res, j); });

        // ── POST /api/save — Save config to disk ──────────────────
        server_->Post("/api/save", [this](const httplib::Request &req, httplib::Response &res)
                      {
            if (!checkAuth(&req, &res)) return;
            manager_.saveConfig();
            jsonResponse(res, {{"success", true}, {"message", "Config saved"}}); });

        // ── GET /api/logs — Recent log lines from ring buffer ─────
        server_->Get("/api/logs", [this](const httplib::Request &req, httplib::Response &res)
                     {
            if (!checkAuth(&req, &res)) return;
            json arr = json::array();
            if (logRingBuffer_) {
                auto lines = logRingBuffer_->last_formatted();
                for (const auto &line : lines) {
                    // Trim trailing newline
                    std::string s = line;
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                        s.pop_back();
                    if (!s.empty())
                        arr.push_back(s);
                }
            }
            jsonResponse(res, arr); });
    }

    // ─────────────────────────────────────────────────────────────────
    // Get local network IP address (for WiFi intranet display)
    // ─────────────────────────────────────────────────────────────────
    std::string WebServer::getLocalIP() const
    {
#ifdef _WIN32
        // Windows: Use GetAdaptersAddresses
        ULONG bufLen = 15000;
        std::vector<BYTE> buf(bufLen);
        auto pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());

        DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                         nullptr, pAddresses, &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW)
        {
            buf.resize(bufLen);
            pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
            ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                       nullptr, pAddresses, &bufLen);
        }

        if (ret == NO_ERROR)
        {
            for (auto adapter = pAddresses; adapter; adapter = adapter->Next)
            {
                if (adapter->OperStatus != IfOperStatusUp)
                    continue;
                if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;

                for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
                {
                    auto sa = reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                    std::string ipStr(ip);
                    // Skip loopback and link-local
                    if (ipStr != "127.0.0.1" && ipStr.substr(0, 4) != "169.")
                    {
                        return ipStr;
                    }
                }
            }
        }
#else
        struct ifaddrs *ifaddr;
        if (getifaddrs(&ifaddr) == 0)
        {
            for (auto ifa = ifaddr; ifa; ifa = ifa->ifa_next)
            {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                    continue;
                auto sa = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                std::string ipStr(ip);
                if (ipStr != "127.0.0.1" && ipStr.substr(0, 4) != "169.")
                {
                    freeifaddrs(ifaddr);
                    return ipStr;
                }
            }
            freeifaddrs(ifaddr);
        }
#endif
        return "127.0.0.1";
    }

} // namespace sm
