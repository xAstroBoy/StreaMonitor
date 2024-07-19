// ─────────────────────────────────────────────────────────────────
// StreaMate site plugin — Naiadsystems manifest API
// Port from Python: GET manifest JSON, extract mp4-hls encodings
// Aliases: pornhublive
// ─────────────────────────────────────────────────────────────────

#include "sites/streamate.h"

namespace sm
{

    REGISTER_SITE(StreaMate);

    StreaMate::StreaMate(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
        sleepOnOffline_ = 10;
        sleepOnRateLimit_ = 60;
        maxConsecutiveErrors_ = 100;
    }

    std::string StreaMate::getWebsiteUrl() const
    {
        return "https://streamate.com/cam/" + username();
    }

    std::string StreaMate::selectBestEncoding() const
    {
        // Extract encodings from formats.mp4-hls.encodings array
        // Select best resolution based on user preference
        if (!lastInfo_.contains("formats"))
            return "";

        auto &formats = lastInfo_["formats"];
        if (!formats.contains("mp4-hls"))
            return "";

        auto &mp4hls = formats["mp4-hls"];
        if (!mp4hls.contains("encodings") || !mp4hls["encodings"].is_array())
            return "";

        std::string bestUrl;
        int bestHeight = 0;

        for (const auto &enc : mp4hls["encodings"])
        {
            std::string location = enc.value("location", "");
            int height = enc.value("videoHeight", 0);

            if (!location.empty() && height > bestHeight)
            {
                bestHeight = height;
                bestUrl = location;
            }
        }

        return bestUrl;
    }

    Status StreaMate::checkStatus()
    {
        std::string url = "https://manifest-server.naiadsystems.com/live/s:" +
                          username() + ".json?last=load&format=mp4-hls";

        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"},
            {"Referer", "https://streamate.com/"}};

        HttpRequest req;
        req.url = url;
        req.timeoutSec = 30;
        req.headers = headers;

        auto resp = http().get(req);

        // Connection error check first — NOT a rate limit!
        if (resp.isConnectionError())
        {
            logger_->warn("Connection error: {}", resp.error);
            setLastError("Connection error: " + resp.error, 0);
            return Status::ConnectionError;
        }

        if (resp.ok())
        {
            try
            {
                lastInfo_ = nlohmann::json::parse(resp.body);
                setLastApiResponse(resp.body);
                return Status::Public;
            }
            catch (const std::exception &e)
            {
                logger_->error("JSON parse error: {}", e.what());
                setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
                return Status::Error;
            }
        }

        if (resp.isNotFound())
        {
            setLastError("Not found (404)", resp.statusCode);
            return Status::NotExist;
        }
        if (resp.statusCode == 403)
        {
            setLastError("Private (403)", resp.statusCode);
            return Status::Private;
        }
        if (resp.isRateLimit())
        {
            setLastError("Rate limited", resp.statusCode);
            return Status::RateLimit;
        }

        logger_->warn("HTTP {} for {}", resp.statusCode, username());
        setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
        return Status::Unknown;
    }

    std::string StreaMate::getVideoUrl()
    {
        std::string url = selectBestEncoding();
        if (url.empty())
            return "";
        return selectResolution(url);
    }

} // namespace sm
