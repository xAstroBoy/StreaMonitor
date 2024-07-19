// ─────────────────────────────────────────────────────────────────
// FanslyLive site plugin — Multi-step account resolution
// ─────────────────────────────────────────────────────────────────

#include "sites/fanslylive.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace sm
{

    REGISTER_SITE(FanslyLive);

    bool FanslyLive::resolveRoomId()
    {
        if (!roomId_.empty())
            return true;

        HttpRequest req;
        req.url = "https://apiv3.fansly.com/api/v1/account?usernames=" +
                  http().urlEncode(username()) + "&ngsw-bypass=true";

        auto resp = http().get(req);
        if (resp.statusCode != 200)
        {
            setLastError("Resolve HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return false;
        }

        try
        {
            auto j = nlohmann::json::parse(resp.body);
            auto response = j.value("response", nlohmann::json::array());
            if (!response.is_array() || response.empty())
                return false;

            for (const auto &acct : response)
            {
                auto uname = acct.value("username", "");
                if (uname == username())
                {
                    roomId_ = acct.value("id", "");
                    return !roomId_.empty();
                }
            }
        }
        catch (const std::exception &e)
        {
            logger_->warn("FanslyLive resolve error: {}", e.what());
        }

        return false;
    }

    Status FanslyLive::checkStatus()
    {
        if (!resolveRoomId())
            return Status::NotExist;

        HttpRequest req;
        req.url = "https://apiv3.fansly.com/api/v1/streaming/channel/" +
                  roomId_ + "?ngsw-bypass=true";

        auto resp = http().get(req);
        if (resp.statusCode != 200)
        {
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::RateLimit;
        }

        try
        {
            auto j = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);
            auto response = j.value("response", nlohmann::json::object());

            if (!response.is_object() || response.is_null())
                return Status::Offline;

            auto stream = response.value("stream", nlohmann::json::object());
            if (stream.is_null() || !stream.is_object())
                return Status::Offline;

            auto status = stream.value("status", 0);
            if (status == 0)
                return Status::Offline;

            bool access = stream.value("access", false);
            hlsUrl_ = stream.value("playbackUrl", "");

            if (status == 2 && access && !hlsUrl_.empty())
                return Status::Public;

            if (status == 2 && (!access || hlsUrl_.empty()))
                return Status::Private;

            return Status::Offline;
        }
        catch (const std::exception &e)
        {
            logger_->warn("FanslyLive status error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::RateLimit;
        }
    }

    std::string FanslyLive::getVideoUrl()
    {
        if (hlsUrl_.empty())
            return {};
        return selectResolution(hlsUrl_);
    }

} // namespace sm
