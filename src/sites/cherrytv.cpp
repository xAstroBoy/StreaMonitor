// ─────────────────────────────────────────────────────────────────
// Cherry.tv site plugin — GraphQL persisted query
// Python-faithful: findStreamerBySlug, correct hash, response paths
// ─────────────────────────────────────────────────────────────────

#include "sites/cherrytv.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace sm
{

    REGISTER_SITE(CherryTV);

    Status CherryTV::checkStatus()
    {
        // Python: operationName=findStreamerBySlug (NOT FindActiveBroadcastBySlug)
        // Python: sha256Hash=1fd980c874484de0b139ef4a67c867200a87f44aa51caf54319e93a4108a7510
        std::string variables = "{\"slug\":\"" + username() + "\"}";
        std::string extensions = "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":"
                                 "\"1fd980c874484de0b139ef4a67c867200a87f44aa51caf54319e93a4108a7510\"}}";

        HttpRequest req;
        req.url = "https://api.cherry.tv/graphql"
                  "?operationName=findStreamerBySlug"
                  "&variables=" +
                  http().urlEncode(variables) + "&extensions=" + http().urlEncode(extensions);

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
            auto data = j.value("data", nlohmann::json::object());

            // Python response path: data.findStreamerBySlug
            if (!data.contains("findStreamerBySlug") ||
                data["findStreamerBySlug"].is_null())
            {
                return Status::NotExist;
            }

            auto streamer = data["findStreamerBySlug"];

            // Python: check for broadcast sub-object
            if (!streamer.contains("broadcast") || streamer["broadcast"].is_null())
                return Status::Offline;

            auto broadcast = streamer["broadcast"];
            // Python field: showStatus (NOT "status")
            auto showStatus = broadcast.value("showStatus", "");

            if (showStatus == "public")
            {
                hlsUrl_ = broadcast.value("hlsUrl", "");
                if (hlsUrl_.empty())
                    return Status::Offline;
                return Status::Public;
            }
            if (showStatus == "private")
                return Status::Private;

            return Status::Offline;
        }
        catch (const std::exception &e)
        {
            logger_->warn("CherryTV JSON error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::RateLimit;
        }
    }

    std::string CherryTV::getVideoUrl()
    {
        if (hlsUrl_.empty())
            return {};
        return selectResolution(hlsUrl_);
    }

} // namespace sm
