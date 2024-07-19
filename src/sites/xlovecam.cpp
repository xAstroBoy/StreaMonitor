// ─────────────────────────────────────────────────────────────────
// XLoveCam site plugin — POST-based form API (Python-faithful)
// Correct: numeric status codes, correct POST body format
// ─────────────────────────────────────────────────────────────────

#include "sites/xlovecam.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace sm
{

    REGISTER_SITE(XLoveCam);

    bool XLoveCam::resolvePerformerId()
    {
        if (!performerId_.empty())
            return true;

        HttpRequest req;
        req.url = "https://www.xlovecam.com/hu/performerAction/onlineList";
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";

        // Python POST body format (different from old C++):
        // config[nickname]={username}&config[favorite]=0&config[recent]=0&config[vip]=0
        // &config[sort][id]=35&offset[from]=0&offset[length]=35&origin=filter-chg&stat=0
        std::string body = "config%5Bnickname%5D=" + http().urlEncode(username()) +
                           "&config%5Bfavorite%5D=0&config%5Brecent%5D=0&config%5Bvip%5D=0"
                           "&config%5Bsort%5D%5Bid%5D=35&offset%5Bfrom%5D=0&offset%5Blength%5D=35"
                           "&origin=filter-chg&stat=0";

        auto resp = http().post(req, body);
        if (resp.statusCode != 200)
        {
            setLastError("Resolve HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return false;
        }

        try
        {
            auto j = nlohmann::json::parse(resp.body);

            // Python: response.performerList is the array of performers
            if (!j.contains("performerList") || !j["performerList"].is_array())
                return false;

            std::string lowerUser = username();
            std::transform(lowerUser.begin(), lowerUser.end(), lowerUser.begin(), ::tolower);

            for (const auto &perf : j["performerList"])
            {
                std::string nick = perf.value("nickname", "");
                std::string lower = nick;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower == lowerUser)
                {
                    // Python: id field
                    if (perf.contains("id") && perf["id"].is_number())
                        performerId_ = std::to_string(perf["id"].get<int>());
                    else if (perf.contains("id") && perf["id"].is_string())
                        performerId_ = perf["id"].get<std::string>();
                    return !performerId_.empty();
                }
            }
        }
        catch (const std::exception &e)
        {
            logger_->warn("XLC performer resolve error: {}", e.what());
        }

        return false;
    }

    Status XLoveCam::checkStatus()
    {
        if (!resolvePerformerId())
            return Status::NotExist;

        HttpRequest req;
        req.url = "https://www.xlovecam.com/hu/performerAction/getPerformerRoom";
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";

        std::string body = "performerId=" + performerId_;
        auto resp = http().post(req, body);
        if (resp.statusCode != 200)
        {
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::RateLimit;
        }

        try
        {
            auto j = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);

            // Python: check performer sub-object exists
            if (!j.contains("performer") || j["performer"].is_null())
                return Status::Unknown;

            auto performer = j["performer"];

            // Python: performer.active = false → NOTEXIST
            bool active = performer.value("active", true);
            if (!active)
                return Status::NotExist;

            // Python: performer.onlineStatus is NUMERIC (0 or 1), NOT string!
            int onlineStatus = 0;
            if (performer.contains("onlineStatus"))
            {
                auto &os = performer["onlineStatus"];
                if (os.is_number())
                    onlineStatus = os.get<int>();
                else if (os.is_string())
                {
                    try
                    {
                        onlineStatus = std::stoi(os.get<std::string>());
                    }
                    catch (...)
                    {
                    }
                }
            }

            if (onlineStatus == 1)
            {
                // Check for HLS URL
                hlsUrl_ = performer.value("hlsPlaylistFree", "");
                if (!hlsUrl_.empty())
                    return Status::Public;
                return Status::Private;
            }

            if (onlineStatus == 0)
                return Status::Offline;

            return Status::Offline;
        }
        catch (const std::exception &e)
        {
            logger_->warn("XLC status error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::RateLimit;
        }
    }

    std::string XLoveCam::getVideoUrl()
    {
        return hlsUrl_; // direct playlist URL
    }

} // namespace sm
