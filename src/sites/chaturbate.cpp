// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Chaturbate implementation
// ─────────────────────────────────────────────────────────────────

#include "sites/chaturbate.h"
#include <regex>
#include <thread>
#include <random>

namespace sm
{

    REGISTER_SITE(Chaturbate);

    Chaturbate::Chaturbate(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
        sleepOnOffline_ = 15;
        sleepOnError_ = 30;
        sleepOnRateLimit_ = 120;
        maxConsecutiveErrors_ = 200;

        backupEndpoints_ = {
            "https://chaturbate.com/get_edge_hls_url_ajax/",
            "https://en.chaturbate.com/get_edge_hls_url_ajax/"};
    }

    std::string Chaturbate::getWebsiteUrl() const
    {
        return "https://www.chaturbate.com/" + username();
    }

    Status Chaturbate::parseRoomStatus(const std::string &roomStatus, bool hasUrl) const
    {
        if (roomStatus == "public")
        {
            return hasUrl ? Status::Public : Status::Restricted;
        }
        if (roomStatus == "private" || roomStatus == "hidden")
            return Status::Private;
        if (roomStatus == "offline")
            return Status::Offline;
        return Status::Offline;
    }

    std::string Chaturbate::buildCmafUrl(const std::string &url) const
    {
        // Convert standard HLS to CMAF:
        // Replace playlist.m3u8 → playlist_sfm4s.m3u8
        // Replace live-XXXamlst → live-c-fhls/amlst
        std::string cmafUrl = url;

        auto pos = cmafUrl.find("playlist.m3u8");
        if (pos != std::string::npos)
            cmafUrl.replace(pos, 13, "playlist_sfm4s.m3u8");

        std::regex liveRe(R"(live-.+amlst)");
        cmafUrl = std::regex_replace(cmafUrl, liveRe, "live-c-fhls/amlst");

        return cmafUrl;
    }

    Status Chaturbate::checkStatus()
    {
        const int maxRetries = 5;
        double baseDelay = 1.0;

        std::map<std::string, std::string> headers = {
            {"X-Requested-With", "XMLHttpRequest"},
            {"Accept", "application/json, text/plain, */*"},
            {"Accept-Language", "en-US,en;q=0.9"},
            {"Cache-Control", "no-cache"},
            {"Pragma", "no-cache"}};

        std::string postBody = "room_slug=" + username() + "&bandwidth=high";

        for (size_t epIdx = 0; epIdx < backupEndpoints_.size(); epIdx++)
        {
            for (int attempt = 0; attempt < maxRetries; attempt++)
            {
                // Exponential backoff with jitter
                if (attempt > 0)
                {
                    static std::mt19937 rng(std::random_device{}());
                    std::uniform_real_distribution<> jitter(0.1, 0.5);
                    double delay = baseDelay * std::pow(2, attempt - 1) + jitter(rng);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(static_cast<int>(delay * 1000)));
                }

                int timeout = std::min(15 + (attempt * 5), 45);

                HttpRequest req;
                req.url = backupEndpoints_[epIdx];
                req.method = "POST";
                req.body = postBody;
                req.contentType = "application/x-www-form-urlencoded";
                req.timeoutSec = timeout;
                req.headers = headers;

                auto resp = http().execute(req);

                if (resp.ok())
                {
                    try
                    {
                        auto json = nlohmann::json::parse(resp.body);
                        lastInfo_ = json;
                        setLastApiResponse(resp.body); // Store for inspection
                        requestFailureCount_ = 0;

                        std::string roomStatus = json.value("room_status", "");
                        std::string url = json.value("url", "");
                        bool cmafEdge = json.value("cmaf_edge", false);

                        // Store for getVideoUrl
                        if (cmafEdge && !url.empty())
                        {
                            lastInfo_["_cmaf_url"] = buildCmafUrl(url);
                        }

                        return parseRoomStatus(roomStatus, !url.empty());
                    }
                    catch (const std::exception &e)
                    {
                        setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
                        logger_->warn("JSON parse error: {}", e.what());
                        if (attempt == maxRetries - 1 && epIdx == backupEndpoints_.size() - 1)
                            return Status::RateLimit;
                        continue;
                    }
                }

                if (resp.isNotFound())
                {
                    setLastError("User not found (404)", resp.statusCode);
                    logger_->info("User not found (404)");
                    return Status::NotExist;
                }

                if (resp.isRateLimit())
                {
                    setLastError("Rate limited (429)", resp.statusCode);
                    logger_->warn("Rate limited (429)");
                    return Status::RateLimit;
                }

                // Connection error (DNS, timeout, etc.) — NOT a rate limit!
                if (resp.isConnectionError())
                {
                    setLastError("Connection error: " + resp.error, 0);
                    logger_->warn("Connection error: {}", resp.error);
                    if (attempt == maxRetries - 1 && epIdx == backupEndpoints_.size() - 1)
                        return Status::ConnectionError;
                    continue;
                }

                if (resp.statusCode == 403 || resp.statusCode == 503)
                {
                    setLastError("Blocked: HTTP " + std::to_string(resp.statusCode), resp.statusCode);
                    logger_->warn("Blocked ({})", resp.statusCode);
                    if (attempt == maxRetries - 1 && epIdx == backupEndpoints_.size() - 1)
                        return Status::RateLimit;
                    continue;
                }

                if (resp.isServerError())
                {
                    setLastError("Server error: HTTP " + std::to_string(resp.statusCode), resp.statusCode);
                    logger_->warn("Server error {}", resp.statusCode);
                    if (attempt == maxRetries - 1 && epIdx == backupEndpoints_.size() - 1)
                        return Status::Error;
                    continue;
                }

                // Other non-OK response (unexpected status code)
                setLastError("Unexpected HTTP " + std::to_string(resp.statusCode), resp.statusCode);
                logger_->warn("Unexpected HTTP {}", resp.statusCode);
                if (attempt == maxRetries - 1 && epIdx == backupEndpoints_.size() - 1)
                    return Status::Error;
            }
        }

        requestFailureCount_++;
        setLastError("All endpoints failed after retries", 0);
        logger_->error("All endpoints failed (failures: {})", requestFailureCount_);
        return Status::ConnectionError;
    }

    std::string Chaturbate::getVideoUrl()
    {
        if (lastInfo_.empty() || !lastInfo_.contains("url"))
            return "";

        std::string url = lastInfo_.value("url", "");
        if (url.empty())
            return "";

        // Use CMAF URL if available
        if (lastInfo_.contains("_cmaf_url"))
            url = lastInfo_["_cmaf_url"].get<std::string>();

        return selectResolution(url);
    }

} // namespace sm
