// ─────────────────────────────────────────────────────────────────
// Flirt4Free site plugin — Room ID lookup + stream URL extraction
// ─────────────────────────────────────────────────────────────────

#include "sites/flirt4free.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <regex>

namespace sm
{

    REGISTER_SITE(Flirt4Free);

    // Static members
    std::mutex Flirt4Free::modelCacheMutex_;
    std::unordered_map<std::string, std::string> Flirt4Free::modelCache_;
    std::chrono::steady_clock::time_point Flirt4Free::lastCacheRefresh_;

    bool Flirt4Free::resolveRoomId()
    {
        if (!roomId_.empty())
            return true;

        // Check cache first
        {
            std::lock_guard lock(modelCacheMutex_);
            std::string lowerUser = username();
            std::transform(lowerUser.begin(), lowerUser.end(), lowerUser.begin(), ::tolower);
            auto it = modelCache_.find(lowerUser);
            if (it != modelCache_.end())
            {
                roomId_ = it->second;
                return true;
            }
        }

        // Fetch the JSON model index (updated from streamonitor mainline)
        HttpRequest req;
        req.url = "https://www.flirt4free.com/?tpl=index2&model=json";
        auto resp = http().get(req);

        // Mark connection errors distinctly (not found in index ≠ connection error)
        if (resp.isConnectionError())
        {
            logger_->warn("Connection error fetching model index: {}", resp.error);
            setLastError("Connection error: " + resp.error, 0);
            lastResolveWasConnectionError_ = true;
            return false;
        }
        lastResolveWasConnectionError_ = false;

        if (resp.statusCode != 200)
            return false;

        try
        {
            // Look for window.__homePageData__ = { ... };
            const std::string marker = "window.__homePageData__ = ";
            auto pos = resp.body.find(marker);
            if (pos == std::string::npos)
                return false;

            // Extract everything after the marker and strip trailing semicolons/whitespace
            auto jsonStr = resp.body.substr(pos + marker.size());
            // Trim trailing whitespace and semicolons
            while (!jsonStr.empty() && (jsonStr.back() == ';' || jsonStr.back() == '\n' ||
                                        jsonStr.back() == '\r' || jsonStr.back() == ' '))
                jsonStr.pop_back();

            auto data = nlohmann::json::parse(jsonStr);

            // New format: { "models": [ ... ] }
            auto modelsArr = data.value("models", nlohmann::json::array());

            // Update cache
            std::lock_guard lock(modelCacheMutex_);
            lastCacheRefresh_ = Clock::now();

            for (const auto &m : modelsArr)
            {
                if (!m.is_object())
                    continue;
                auto seoName = m.value("model_seo_name", "");
                if (seoName.empty())
                    continue;

                // model_id can be string or int
                std::string modelIdStr;
                if (m.contains("model_id"))
                {
                    if (m["model_id"].is_string())
                        modelIdStr = m["model_id"].get<std::string>();
                    else
                        modelIdStr = std::to_string(m["model_id"].get<int64_t>());
                }
                if (modelIdStr.empty())
                    continue;

                std::string lower = seoName;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                modelCache_[lower] = modelIdStr;
            }

            // Look up our model
            std::string lowerUser = username();
            std::transform(lowerUser.begin(), lowerUser.end(), lowerUser.begin(), ::tolower);
            auto it = modelCache_.find(lowerUser);
            if (it != modelCache_.end())
            {
                roomId_ = it->second;

                // Update username to canonical casing from model_seo_name
                for (const auto &m : modelsArr)
                {
                    if (!m.is_object()) continue;
                    auto seoName = m.value("model_seo_name", "");
                    std::string seoLower = seoName;
                    std::transform(seoLower.begin(), seoLower.end(), seoLower.begin(), ::tolower);
                    if (seoLower == lowerUser && seoName != username())
                    {
                        logger_->info("Username case fix: {} → {}", username(), seoName);
                        setUsername(seoName);
                        break;
                    }
                }

                return true;
            }
        }
        catch (const std::exception &e)
        {
            logger_->warn("F4F room ID resolve error: {}", e.what());
        }

        return false;
    }

    Status Flirt4Free::checkStatus()
    {
        if (!resolveRoomId())
        {
            // If resolve failed due to connection error, report that specifically
            if (lastResolveWasConnectionError_)
                return Status::ConnectionError;
            // Model not in the index = offline (not an error)
            return Status::Offline;
        }

        // Get stream URLs
        HttpRequest req;
        // Python: get-stream-urls.php (NOT stream-urls.php)
        req.url = "https://www.flirt4free.com/ws/chat/get-stream-urls.php?model_id=" + roomId_;
        auto resp = http().get(req);

        // Connection error check first
        if (resp.isConnectionError())
        {
            logger_->warn("Connection error: {}", resp.error);
            setLastError("Connection error: " + resp.error, 0);
            return Status::ConnectionError;
        }
        if (resp.isRateLimit())
        {
            setLastError("Rate limited", resp.statusCode);
            return Status::RateLimit;
        }
        if (resp.statusCode != 200)
        {
            logger_->warn("Stream URLs HTTP {}", resp.statusCode);
            setLastError("Stream URLs HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::Error;
        }

        try
        {
            auto j = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);

            // Extract HLS URL
            hlsUrl_.clear();
            if (j.contains("formats") && j["formats"].is_object())
            {
                auto &formats = j["formats"];
                if (formats.contains("mp4-hls") && formats["mp4-hls"].is_object())
                {
                    hlsUrl_ = formats["mp4-hls"].value("url", "");
                }
            }

            if (hlsUrl_.empty())
                return Status::Offline;

            // Prefix https: if needed
            if (hlsUrl_.find("//") == 0)
                hlsUrl_ = "https:" + hlsUrl_;
        }
        catch (const std::exception &e)
        {
            logger_->warn("F4F stream URL error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::Error;
        }

        // Get room status
        HttpRequest statusReq;
        // Python: chat-room-interface.php with a=login_room (NOT room-interface.php)
        statusReq.url = "https://www.flirt4free.com/ws/rooms/chat-room-interface.php?a=login_room&model_id=" + roomId_;
        auto statusResp = http().get(statusReq);

        // Connection error check first
        if (statusResp.isConnectionError())
        {
            logger_->warn("Connection error: {}", statusResp.error);
            setLastError("Connection error: " + statusResp.error, 0);
            return Status::ConnectionError;
        }
        if (statusResp.isRateLimit())
        {
            setLastError("Rate limited", statusResp.statusCode);
            return Status::RateLimit;
        }
        if (statusResp.statusCode != 200)
        {
            logger_->warn("Room status HTTP {}", statusResp.statusCode);
            setLastError("Room status HTTP " + std::to_string(statusResp.statusCode), statusResp.statusCode);
            return Status::Error;
        }

        try
        {
            auto sj = nlohmann::json::parse(statusResp.body);

            // Python JSON path: config.room.status
            auto config = sj.value("config", nlohmann::json::object());
            auto room = config.value("room", nlohmann::json::object());
            auto roomStatus = room.value("status", "");

            // Python: HTTP 404 = NOTEXIST (check show_status == 44)
            int showStatus = room.value("show_status", 0);
            if (showStatus == 44)
                return Status::NotExist;

            if (roomStatus == "O")
                return Status::Public;
            if (roomStatus == "P")
                return Status::Private;
            if (roomStatus == "F")
                return Status::Offline;

            return Status::Offline;
        }
        catch (const std::exception &e)
        {
            logger_->warn("F4F status error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), statusResp.statusCode);
            return Status::Error;
        }
    }

    std::string Flirt4Free::getVideoUrl()
    {
        if (hlsUrl_.empty())
            return {};
        return selectResolution(hlsUrl_);
    }

} // namespace sm
