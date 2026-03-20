// ─────────────────────────────────────────────────────────────────
// DreamCam site plugin — Simple REST API
// ─────────────────────────────────────────────────────────────────

#include "sites/dreamcam.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace sm
{

    REGISTER_SITE(DreamCam);

    Status DreamCam::checkStatus()
    {
        HttpRequest req;
        // Python: https://bss.dreamcamtrue.com/api/clients/v1/broadcasts/models/{username}
        req.url = "https://bss.dreamcamtrue.com/api/clients/v1/broadcasts/models/" + username();

        auto resp = http().get(req);

        // Connection error check first — NOT a rate limit!
        if (resp.isConnectionError())
        {
            logger_->warn("Connection error: {}", resp.error);
            setLastError("Connection error: " + resp.error, 0);
            return Status::ConnectionError;
        }
        if (resp.statusCode == 404)
        {
            setLastError("Not found (404)", resp.statusCode);
            return Status::NotExist;
        }
        if (resp.isRateLimit())
        {
            setLastError("Rate limited", resp.statusCode);
            return Status::RateLimit;
        }
        if (resp.statusCode != 200)
        {
            logger_->warn("HTTP {}", resp.statusCode);
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::Error;
        }

        try
        {
            auto j = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);
            auto broadcastStatus = j.value("broadcastStatus", "offline");

            if (broadcastStatus == "public")
            {
                // Extract stream URL from streams array
                streamUrl_.clear();
                if (j.contains("streams") && j["streams"].is_array())
                {
                    for (const auto &stream : j["streams"])
                    {
                        auto type = stream.value("streamType", "");
                        auto url = stream.value("url", "");
                        if (type == streamType_ && !url.empty())
                        {
                            streamUrl_ = url;
                            break;
                        }
                    }
                }
                return streamUrl_.empty() ? Status::Offline : Status::Public;
            }
            if (broadcastStatus == "private")
                return Status::Private;
            if (broadcastStatus == "away")
                return Status::Offline;
            return Status::Offline;
        }
        catch (const std::exception &e)
        {
            logger_->warn("DreamCam JSON error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::Error;
        }
    }

    std::string DreamCam::getVideoUrl()
    {
        if (streamUrl_.empty())
            return {};
        return selectResolution(streamUrl_);
    }

} // namespace sm
