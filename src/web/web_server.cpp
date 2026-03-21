// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Embedded Web Server Implementation
// REST API + static file serving for the Next.js dashboard
// HTTP/2 multiplexing — unlimited concurrent preview streams
// ─────────────────────────────────────────────────────────────────

#include "web/web_server.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <random>
#include <iomanip>
#include <thread>
#include <chrono>

// stb_image_write — JPEG encoding for preview frames
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO // we only use the callback API
#include "stb_image_write.h"

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
using Clock = std::chrono::steady_clock;

namespace sm
{

    WebServer::WebServer(BotManager &manager, AppConfig &config,
                         ModelConfigStore &configStore)
        : manager_(manager), config_(config), configStore_(configStore)
    {
        // Create server (plain HTTP — no certs needed)
        server_ = std::make_unique<H2Server>("", "");
        spdlog::info("Web server created (HTTP)");
    }

    WebServer::~WebServer()
    {
        stop();
    }

    bool WebServer::start()
    {
        if (running_.load() || !server_)
            return false;

        setupCORS();
        setupRoutes();
        setupStaticFiles();

        if (!server_->listenHttp(config_.webHost, config_.webPort))
        {
            spdlog::error("Web server failed to start on {}:{}", config_.webHost, config_.webPort);
            return false;
        }

        running_.store(true);
        spdlog::info("Web server started on {}:{}", config_.webHost, config_.webPort);
        spdlog::info("  Local:   {}", getLocalUrl());
        spdlog::info("  Network: {}", getNetworkUrl());
        spdlog::info("  Protocol: HTTP/1.1 (all devices, no cert needed)");

        return true;
    }

    void WebServer::stop()
    {
        if (!running_.load())
            return;

        if (server_)
            server_->stop();

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
        server_->set_default_headers({{"access-control-allow-origin", "*"},
                                      {"access-control-allow-methods", "GET, POST, PUT, DELETE, OPTIONS"},
                                      {"access-control-allow-headers", "Content-Type, Authorization"}});

        // Handle preflight OPTIONS requests
        server_->Options(".*", [](const H2Request &, H2Response &res)
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
            auto exePath = std::filesystem::current_path();
            staticDir = exePath / staticDir;
        }

        if (std::filesystem::exists(staticDir))
        {
            server_->set_mount_point("/", staticDir.string());
            spdlog::info("Serving static files from: {}", staticDir.string());

            // SPA fallback: serve index.html for non-API, non-file routes
            server_->set_error_handler([staticDir](const H2Request &req, H2Response &res)
                                       {
                if (res.status == 404 && req.path.substr(0, 4) != "/api") {
                    auto indexPath = staticDir / "index.html";
                    if (std::filesystem::exists(indexPath)) {
                        std::ifstream ifs(indexPath.string());
                        if (ifs.is_open()) {
                            std::string content((std::istreambuf_iterator<char>(ifs)),
                                               std::istreambuf_iterator<char>());
                            res.set_content(content, "text/html; charset=utf-8");
                            res.status = 200;
                        }
                    }
                } });
        }
        else
        {
            spdlog::warn("Static web directory not found: {}. Dashboard unavailable.", staticDir.string());

            server_->Get("^/$", [](const H2Request &, H2Response &res)
                         { res.set_content(R"html(<!DOCTYPE html>
<html><head><title>StreaMonitor</title></head>
<body style="background:#0a0a0a;color:#fff;font-family:system-ui;display:flex;align-items:center;justify-content:center;height:100vh;margin:0">
<div style="text-align:center">
<h1>🎬 StreaMonitor v2.0</h1>
<p>API is running (HTTP/2). Dashboard static files not found.</p>
<p>Place the Next.js export in the <code>web/</code> directory.</p>
<p><a href="/api/status" style="color:#60a5fa">API Status →</a></p>
</div></body></html>)html",
                                           "text/html; charset=utf-8"); });
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Helper: RGBA → JPEG conversion (via stb_image_write)
    // ─────────────────────────────────────────────────────────────────
    static void jpegWriteFunc_(void *context, void *data, int size)
    {
        auto *buf = static_cast<std::string *>(context);
        buf->append(static_cast<const char *>(data), static_cast<size_t>(size));
    }

    static std::string rgbaToJpeg(const uint8_t *rgba, int w, int h, int quality = 80)
    {
        std::string result;
        result.reserve(static_cast<size_t>(w) * h / 8);
        stbi_write_jpg_to_func(jpegWriteFunc_, &result, w, h, 4, rgba, quality);
        return result;
    }

    // ─────────────────────────────────────────────────────────────────
    // Helper: JSON response
    // ─────────────────────────────────────────────────────────────────
    static void jsonResponse(H2Response &res, const json &j, int status = 200)
    {
        res.set_content(j.dump(), "application/json");
        res.status = status;
    }

    static void jsonError(H2Response &res, const std::string &msg, int status = 400)
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

    bool WebServer::checkAuth(const H2Request &req, H2Response &res)
    {
        // Check Authorization header: "Bearer <token>"
        auto authHeader = req.get_header_value("authorization");
        if (authHeader.size() > 7 && authHeader.substr(0, 7) == "Bearer ")
        {
            std::string token = authHeader.substr(7);
            if (validateToken(token))
                return true;
        }

        // Check cookie
        auto cookieHeader = req.get_header_value("cookie");
        if (!cookieHeader.empty())
        {
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
        server_->Post("^/api/auth/login$", [this](const H2Request &req, H2Response &res)
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
                    res.set_header("set-cookie", "sm_token=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=86400");
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
        server_->Post("^/api/auth/logout$", [this](const H2Request &req, H2Response &res)
                      {
            auto authHeader = req.get_header_value("authorization");
            if (authHeader.size() > 7 && authHeader.substr(0, 7) == "Bearer ") {
                std::string token = authHeader.substr(7);
                std::lock_guard lock(tokenMutex_);
                validTokens_.erase(token);
            }
            auto cookieHeader = req.get_header_value("cookie");
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
            res.set_header("set-cookie", "sm_token=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
            json j = {{"success", true}};
            jsonResponse(res, j); });

        // ── GET /api/auth/check — Validate current session ────────
        server_->Get("^/api/auth/check$", [this](const H2Request &req, H2Response &res)
                     {
            if (checkAuth(req, res)) {
                json j = {{"authenticated", true}, {"username", config_.webUsername}};
                jsonResponse(res, j);
            } });

        // ── PUT /api/auth/credentials — Update login credentials ──
        server_->Put("^/api/auth/credentials$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
            try {
                auto body = json::parse(req.body);
                std::string newUser = body.value("username", "");
                std::string newPass = body.value("password", "");
                std::string currentPass = body.value("currentPassword", "");

                if (currentPass != config_.webPassword) {
                    jsonError(res, "Current password incorrect", 403);
                    return;
                }

                if (!newUser.empty()) config_.webUsername = newUser;
                if (!newPass.empty()) config_.webPassword = newPass;

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
        server_->Get("^/api/status$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
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
                {"protocol", "h2"},
                {"disk", {
                    {"totalBytes", disk.totalBytes},
                    {"freeBytes", disk.freeBytes},
                    {"downloadDirBytes", disk.downloadDirBytes},
                    {"fileCount", disk.fileCount}
                }}
            };
            jsonResponse(res, j); });

        // ── GET /api/models — All models with states ──────────────
        server_->Get("^/api/models$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
            auto states = manager_.getAllStates();
            json arr = json::array();
            for (const auto &st : states) {
                arr.push_back(botStateToJson(st));
            }
            jsonResponse(res, arr); });

        // ── GET /api/preview/:username/:site — JPEG snapshot ──────
        // Non-blocking: returns latest frame or 404 if none available
        server_->Get(R"(/api/preview/([^/]+)/([^/]+))",
                     [this](const H2Request &req, H2Response &res)
                     {
                         std::string username = req.matches[1];
                         std::string site = req.matches[2];

                         uint64_t version = 0;
                         PreviewFrame frame;
                         if (!manager_.consumePreview(username, site, frame, version))
                         {
                             res.status = 404;
                             res.set_content("No preview available", "text/plain");
                             return;
                         }

                         std::string jpg = rgbaToJpeg(frame.pixels.data(), frame.width, frame.height);
                         res.set_header("cache-control", "no-cache, no-store, must-revalidate");
                         res.set_content(jpg, "image/jpeg");
                     });

        // ── GET /api/stream/:username/:site — MJPEG live stream ───
        server_->Get(R"(/api/stream/([^/]+)/([^/]+))",
                     [this](const H2Request &req, H2Response &res)
                     {
                         std::string username = req.matches[1];
                         std::string site = req.matches[2];
                         const std::string boundary = "frame";

                         res.set_header("cache-control", "no-cache, no-store, must-revalidate");
                         res.set_header("access-control-allow-origin", "*");

                         res.set_content_provider(
                             "multipart/x-mixed-replace; boundary=" + boundary,
                             [this, username, site, boundary,
                              lastVersion = uint64_t(0),
                              lastRecordingCheck = Clock::now()](size_t, H2DataSink &sink) mutable -> bool
                             {
                                 if (!sink.is_writable())
                                     return false;

                                 // Check recording status periodically
                                 auto state = manager_.getBotState(username, site);
                                 if (!state || !state->recording)
                                 {
                                     auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                                        Clock::now() - lastRecordingCheck)
                                                        .count();
                                     if (elapsed > 5)
                                         return false; // Stop stream after 5s of not-recording
                                     std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                     return true; // No data, but keep alive
                                 }
                                 lastRecordingCheck = Clock::now();

                                 // Blocking wait for next frame (up to 200ms)
                                 PreviewFrame frame;
                                 if (!manager_.waitForPreview(username, site, frame, lastVersion, 200))
                                     return true; // Timed out, keep alive

                                 std::string jpg = rgbaToJpeg(frame.pixels.data(), frame.width, frame.height);

                                 std::string header = "--" + boundary + "\r\n"
                                                                        "Content-Type: image/jpeg\r\n"
                                                                        "Content-Length: " +
                                                      std::to_string(jpg.size()) + "\r\n\r\n";

                                 if (!sink.write(header.data(), header.size()))
                                     return false;
                                 if (!sink.write(jpg.data(), jpg.size()))
                                     return false;
                                 std::string crlf = "\r\n";
                                 sink.write(crlf.data(), crlf.size());
                                 return true;
                             },
                             [](bool) { /* cleanup */ });
                     });

        // ── POST /api/models — Add a model ────────────────────────
        server_->Post("^/api/models$", [this](const H2Request &req, H2Response &res)
                      {
            if (!checkAuth(req, res)) return;
            try {
                auto body = json::parse(req.body);
                std::string username = body.value("username", "");
                std::string site = body.value("site", "");
                bool autoStart = body.value("autoStart", true);

                if (username.empty() || site.empty()) {
                    jsonError(res, "username and site are required");
                    return;
                }

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

        // ── DELETE /api/models/:username/:site ─────────────────────
        server_->Delete(R"(/api/models/([^/]+)/([^/]+))",
                        [this](const H2Request &req, H2Response &res)
                        {
                            if (!checkAuth(req, res))
                                return;
                            std::string username = req.matches[1];
                            std::string site = req.matches[2];

                            if (manager_.removeBot(username, site))
                                jsonResponse(res, {{"success", true}, {"message", "Model removed"}});
                            else
                                jsonError(res, "Model not found", 404);
                        });

        // ── POST /api/models/:username/:site/start ────────────────
        server_->Post(R"(/api/models/([^/]+)/([^/]+)/start)",
                      [this](const H2Request &req, H2Response &res)
                      {
                          if (!checkAuth(req, res))
                              return;
                          if (manager_.startBot(req.matches[1], req.matches[2]))
                              jsonResponse(res, {{"success", true}});
                          else
                              jsonError(res, "Model not found", 404);
                      });

        // ── POST /api/models/:username/:site/stop ─────────────────
        server_->Post(R"(/api/models/([^/]+)/([^/]+)/stop)",
                      [this](const H2Request &req, H2Response &res)
                      {
                          if (!checkAuth(req, res))
                              return;
                          if (manager_.stopBot(req.matches[1], req.matches[2]))
                              jsonResponse(res, {{"success", true}});
                          else
                              jsonError(res, "Model not found", 404);
                      });

        // ── POST /api/models/:username/:site/restart ──────────────
        server_->Post(R"(/api/models/([^/]+)/([^/]+)/restart)",
                      [this](const H2Request &req, H2Response &res)
                      {
                          if (!checkAuth(req, res))
                              return;
                          if (manager_.restartBot(req.matches[1], req.matches[2]))
                              jsonResponse(res, {{"success", true}});
                          else
                              jsonError(res, "Model not found", 404);
                      });

        // ── POST /api/start-all ───────────────────────────────────
        server_->Post("^/api/start-all$", [this](const H2Request &req, H2Response &res)
                      {
            if (!checkAuth(req, res)) return;
            manager_.startAllBots();
            manager_.startAllGroups();
            jsonResponse(res, {{"success", true}, {"message", "All bots and groups started"}}); });

        // ── POST /api/stop-all ────────────────────────────────────
        server_->Post("^/api/stop-all$", [this](const H2Request &req, H2Response &res)
                      {
            if (!checkAuth(req, res)) return;
            manager_.stopAllGroups();
            manager_.stopAll();
            jsonResponse(res, {{"success", true}, {"message", "All bots and groups stopped"}}); });

        // ── GET /api/sites — Available sites ──────────────────────
        server_->Get("^/api/sites$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
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

        // ── GET /api/groups ────────────────────────────────────────
        server_->Get("^/api/groups$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
            auto groups = manager_.getCrossRegisterGroups();
            auto groupStates = manager_.getAllGroupStates();
            json arr = json::array();
            for (const auto &g : groups) {
                json members = json::array();
                for (const auto &[site, user] : g.members) {
                    members.push_back({{"site", site}, {"username", user}});
                }
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
        server_->Post("^/api/groups$", [this](const H2Request &req, H2Response &res)
                      {
            if (!checkAuth(req, res)) return;
            try {
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
                        [this](const H2Request &req, H2Response &res)
                        {
                            if (!checkAuth(req, res))
                                return;
                            if (manager_.removeCrossRegisterGroup(req.matches[1]))
                                jsonResponse(res, {{"success", true}});
                            else
                                jsonError(res, "Group not found", 404);
                        });

        // ── POST /api/groups/:name/start ──────────────────────────
        server_->Post(R"(/api/groups/([^/]+)/start)",
                      [this](const H2Request &req, H2Response &res)
                      {
                          if (!checkAuth(req, res))
                              return;
                          if (manager_.startGroup(req.matches[1]))
                              jsonResponse(res, {{"success", true}});
                          else
                              jsonError(res, "Group not found or already running", 404);
                      });

        // ── POST /api/groups/:name/stop ───────────────────────────
        server_->Post(R"(/api/groups/([^/]+)/stop)",
                      [this](const H2Request &req, H2Response &res)
                      {
                          if (!checkAuth(req, res))
                              return;
                          if (manager_.stopGroup(req.matches[1]))
                              jsonResponse(res, {{"success", true}});
                          else
                              jsonError(res, "Group not found or not running", 404);
                      });

        // ── POST /api/groups/:name/members ────────────────────────
        server_->Post(R"(/api/groups/([^/]+)/members)",
                      [this](const H2Request &req, H2Response &res)
                      {
                          if (!checkAuth(req, res))
                              return;
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
                              if (manager_.addToCrossRegisterGroup(req.matches[1], site, username))
                                  jsonResponse(res, {{"success", true}});
                              else
                                  jsonError(res, "Failed to add member");
                          }
                          catch (const std::exception &e)
                          {
                              jsonError(res, std::string("Invalid JSON: ") + e.what());
                          }
                      });

        // ── DELETE /api/groups/:name/members ──────────────────────
        server_->Delete(R"(/api/groups/([^/]+)/members)",
                        [this](const H2Request &req, H2Response &res)
                        {
                            if (!checkAuth(req, res))
                                return;
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
                                if (manager_.removeFromCrossRegisterGroup(req.matches[1], site, username))
                                    jsonResponse(res, {{"success", true}});
                                else
                                    jsonError(res, "Failed to remove member");
                            }
                            catch (const std::exception &e)
                            {
                                jsonError(res, std::string("Invalid JSON: ") + e.what());
                            }
                        });

        // ── GET /api/config ───────────────────────────────────────
        server_->Get("^/api/config$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
            json j = {
                {"downloadsDir", config_.downloadsDir.string()},
                {"container", config_.container == ContainerFormat::MKV ? "mkv" :
                              config_.container == ContainerFormat::MP4 ? "mp4" : "ts"},
                {"wantedResolution", config_.wantedResolution},
                {"filenameFormat", config_.filenameFormat},
                {"autoRemoveNonExistent", config_.autoRemoveNonExistent},
                {"logLevel", config_.logLevel},
                {"webEnabled", config_.webEnabled},
                {"webHost", config_.webHost},
                {"webPort", config_.webPort},
                {"debug", config_.debug},
                {"enablePreviewCapture", config_.enablePreviewCapture},
                {"minFreeDiskPercent", config_.minFreeDiskPercent},
                {"httpTimeoutSec", config_.httpTimeoutSec},
                {"userAgent", config_.userAgent},
                {"spyPrivateEnabled", config_.spyPrivateEnabled},
                {"stripchatCookiesSet", !config_.stripchatCookies.empty()}
            };
            jsonResponse(res, j); });

        // ── PUT /api/config ───────────────────────────────────────
        server_->Put("^/api/config$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
            try {
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
                if (body.contains("enablePreviewCapture"))
                    config_.enablePreviewCapture = body["enablePreviewCapture"].get<bool>();
                if (body.contains("filenameFormat"))
                    config_.filenameFormat = body["filenameFormat"].get<std::string>();
                if (body.contains("autoRemoveNonExistent"))
                    config_.autoRemoveNonExistent = body["autoRemoveNonExistent"].get<bool>();
                if (body.contains("logLevel"))
                {
                    config_.logLevel = body["logLevel"].get<std::string>();
                    spdlog::level::level_enum lvl = spdlog::level::info;
                    if (config_.logLevel == "debug") lvl = spdlog::level::debug;
                    else if (config_.logLevel == "warn") lvl = spdlog::level::warn;
                    else if (config_.logLevel == "error") lvl = spdlog::level::err;
                    spdlog::set_level(lvl);
                    spdlog::apply_all([lvl](std::shared_ptr<spdlog::logger> l) { l->set_level(lvl); });
                }
                if (body.contains("spyPrivateEnabled"))
                    config_.spyPrivateEnabled = body["spyPrivateEnabled"].get<bool>();
                if (body.contains("stripchatCookies"))
                    config_.stripchatCookies = body["stripchatCookies"].get<std::string>();

                jsonResponse(res, {{"success", true}, {"message", "Config updated"}});
            } catch (const std::exception &e) {
                jsonError(res, std::string("Invalid JSON: ") + e.what());
            } });

        // ── GET /api/disk ─────────────────────────────────────────
        server_->Get("^/api/disk$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
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

        // ── POST /api/save ────────────────────────────────────────
        server_->Post("^/api/save$", [this](const H2Request &req, H2Response &res)
                      {
            if (!checkAuth(req, res)) return;
            manager_.saveConfig();
            jsonResponse(res, {{"success", true}, {"message", "Config saved"}}); });

        // ── POST /api/import-config — Import Python SM config (Issue #45)
        server_->Post("^/api/import-config$", [this](const H2Request &req, H2Response &res)
                      {
            if (!checkAuth(req, res)) return;
            if (req.body.empty()) {
                jsonError(res, "Request body must contain the Python SM config.json content");
                return;
            }
            auto result = manager_.importFromPythonConfig(req.body);
            json j = {
                {"success", true},
                {"imported", result.imported},
                {"skipped", result.skipped},
                {"failed", result.failed},
                {"errors", result.errors},
                {"message", std::to_string(result.imported) + " models imported, " +
                            std::to_string(result.skipped) + " skipped, " +
                            std::to_string(result.failed) + " failed"}
            };
            jsonResponse(res, j, result.imported > 0 ? 200 : 200); });

        // ── GET /api/logs ─────────────────────────────────────────
        server_->Get("^/api/logs$", [this](const H2Request &req, H2Response &res)
                     {
            if (!checkAuth(req, res)) return;
            json arr = json::array();
            if (logRingBuffer_) {
                auto lines = logRingBuffer_->last_formatted();
                for (const auto &line : lines) {
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
    // Get local network IP address (prefer interfaces with gateway)
    // ─────────────────────────────────────────────────────────────────
    std::string WebServer::getLocalIP() const
    {
#ifdef _WIN32
        ULONG bufLen = 15000;
        std::vector<BYTE> buf(bufLen);
        auto pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());

        DWORD ret = GetAdaptersAddresses(AF_INET,
                                         GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS,
                                         nullptr, pAddresses, &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW)
        {
            buf.resize(bufLen);
            pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
            ret = GetAdaptersAddresses(AF_INET,
                                       GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS,
                                       nullptr, pAddresses, &bufLen);
        }

        if (ret == NO_ERROR)
        {
            std::string ipWithGateway;
            std::string fallbackIp;

            for (auto adapter = pAddresses; adapter; adapter = adapter->Next)
            {
                if (adapter->OperStatus != IfOperStatusUp)
                    continue;
                if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;

                bool hasGateway = false;
                for (auto gw = adapter->FirstGatewayAddress; gw; gw = gw->Next)
                {
                    if (gw->Address.lpSockaddr->sa_family == AF_INET)
                    {
                        auto gwSa = reinterpret_cast<sockaddr_in *>(gw->Address.lpSockaddr);
                        if (gwSa->sin_addr.s_addr != 0)
                        {
                            hasGateway = true;
                            break;
                        }
                    }
                }

                for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
                {
                    if (unicast->Address.lpSockaddr->sa_family != AF_INET)
                        continue;

                    auto sa = reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                    std::string ipStr(ip);

                    if (ipStr == "127.0.0.1" || ipStr.substr(0, 4) == "169.")
                        continue;

                    if (hasGateway && ipWithGateway.empty())
                    {
                        ipWithGateway = ipStr;
                    }
                    else if (fallbackIp.empty())
                    {
                        fallbackIp = ipStr;
                    }
                }
            }

            if (!ipWithGateway.empty())
                return ipWithGateway;
            if (!fallbackIp.empty())
                return fallbackIp;
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
