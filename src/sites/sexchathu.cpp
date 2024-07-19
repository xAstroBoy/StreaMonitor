// ─────────────────────────────────────────────────────────────────
// SexChatHU site plugin — Adult Performer Network
// Port from Python: SCHUModelInfo, RoomIdBot, bulk_update,
// performer list caching, getRoom API
// ─────────────────────────────────────────────────────────────────

#include "sites/sexchathu.h"
#include <algorithm>

namespace sm
{

    REGISTER_SITE(SexChatHU);

    SexChatHU::CachedPerformerList &SexChatHU::getCache()
    {
        static CachedPerformerList cache;
        return cache;
    }

    nlohmann::json SexChatHU::fetchPerformerList(HttpClient &http)
    {
        auto &cache = getCache();
        std::lock_guard<std::mutex> lock(cache.mutex);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - cache.fetchTime);

        // Cache for 1 hour
        if (!cache.data.is_null() && elapsed.count() < 60)
            return cache.data;

        auto resp = http.get("https://sexchat.hu/ajax/api/roomList/babes", 30);
        if (resp.ok())
        {
            try
            {
                cache.data = nlohmann::json::parse(resp.body);
                cache.fetchTime = now;
                return cache.data;
            }
            catch (...)
            {
            }
        }

        return cache.data; // Return stale cache on failure
    }

    std::optional<std::string> SexChatHU::findRoomIdFromList(
        const std::string &username, HttpClient &http,
        std::shared_ptr<spdlog::logger> logger)
    {
        auto performers = fetchPerformerList(http);
        if (!performers.is_array())
            return std::nullopt;

        // Case-insensitive username search
        std::string lowerUser = username;
        std::transform(lowerUser.begin(), lowerUser.end(), lowerUser.begin(), ::tolower);

        for (const auto &perf : performers)
        {
            if (!perf.is_object())
                continue;

            std::string screenname = perf.value("screenname", "");
            std::string lowerScreenname = screenname;
            std::transform(lowerScreenname.begin(), lowerScreenname.end(),
                           lowerScreenname.begin(), ::tolower);

            if (lowerScreenname == lowerUser)
            {
                auto perfid = perf.value("perfid", 0);
                if (perfid > 0)
                    return std::to_string(perfid);
            }
        }

        return std::nullopt;
    }

    SexChatHU::SexChatHU(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
        sleepOnOffline_ = 10;
        sleepOnRateLimit_ = 60;
        maxConsecutiveErrors_ = 100;

        // Try to resolve room_id from username
        if (username.find_first_not_of("0123456789") == std::string::npos)
        {
            // Username is numeric — use as room_id directly
            roomId_ = username;
        }
        else
        {
            auto resolved = findRoomIdFromList(username, http(), logger_);
            if (resolved)
                roomId_ = *resolved;
        }
    }

    std::string SexChatHU::getWebsiteUrl() const
    {
        if (roomId_.empty())
            return "https://sexchat.hu/";
        return "https://sexchat.hu/mypage/" + roomId_ + "/" + username() + "/chat";
    }

    Status SexChatHU::checkStatus()
    {
        if (roomId_.empty())
            return Status::NotExist;

        std::string url = "https://chat.a.apn2.com/chat-api/index.php/room/getRoom"
                          "?tokenID=guest&roomID=" +
                          roomId_;

        auto resp = http().get(url, 30);

        if (resp.isNotFound())
        {
            setLastError("Not found (404)", resp.statusCode);
            return Status::NotExist;
        }
        if (!resp.ok())
        {
            logger_->warn("HTTP {} for {}", resp.statusCode, username());
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::Unknown;
        }

        try
        {
            lastInfo_ = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);

            // Check if performer is active
            bool active = lastInfo_.value("active", false);
            if (!active)
                return Status::NotExist;

            // Get online status
            std::string onlineStatus = lastInfo_.value("onlineStatus", "");
            std::transform(onlineStatus.begin(), onlineStatus.end(),
                           onlineStatus.begin(), ::tolower);

            if (onlineStatus == "free")
            {
                // Check if HLS stream is available
                auto onlineParams = lastInfo_.value("onlineParams", nlohmann::json::object());
                auto modeSpecific = onlineParams.value("modeSpecific", nlohmann::json::object());
                auto main = modeSpecific.value("main", nlohmann::json::object());

                if (main.contains("hls"))
                    return Status::Public;
                return Status::Private;
            }

            if (onlineStatus == "vip" || onlineStatus == "group" || onlineStatus == "priv")
                return Status::Private;

            if (onlineStatus == "offline")
                return Status::Offline;

            return Status::Unknown;
        }
        catch (const std::exception &e)
        {
            logger_->error("Parse error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::Error;
        }
    }

    std::string SexChatHU::getVideoUrl()
    {
        if (lastInfo_.empty())
            return "";

        try
        {
            auto onlineParams = lastInfo_.value("onlineParams", nlohmann::json::object());
            auto modeSpecific = onlineParams.value("modeSpecific", nlohmann::json::object());
            auto main = modeSpecific.value("main", nlohmann::json::object());
            auto hls = main.value("hls", nlohmann::json::object());
            std::string address = hls.value("address", "");

            if (address.empty())
                return "";

            // Ensure URL starts with https
            if (address.substr(0, 4) != "http")
                address = "https:" + address;

            return selectResolution(address);
        }
        catch (const std::exception &e)
        {
            logger_->error("Error getting video URL: {}", e.what());
            return "";
        }
    }

} // namespace sm
