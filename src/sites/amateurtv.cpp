// ─────────────────────────────────────────────────────────────────
// AmateurTV site plugin — REST API
// Port from Python: ATVModelInfo, fmp4 quality selection
// ─────────────────────────────────────────────────────────────────

#include "sites/amateurtv.h"
#include <sstream>

namespace sm
{

    REGISTER_SITE(AmateurTV);

    AmateurTV::AmateurTV(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
        sleepOnOffline_ = 10;
        sleepOnRateLimit_ = 60;
        maxConsecutiveErrors_ = 80;
    }

    std::string AmateurTV::getWebsiteUrl() const
    {
        return "https://amateur.tv/" + username();
    }

    std::string AmateurTV::selectBestQuality() const
    {
        // Parse qualities array (e.g. ["640x480", "1280x720", "1920x1080"])
        // and pick the best matching the user's resolution preference
        if (!lastInfo_.contains("qualities") || !lastInfo_["qualities"].is_array())
            return "";
        if (!lastInfo_.contains("videoTechnologies") || !lastInfo_["videoTechnologies"].is_object())
            return "";

        std::string fmp4Url = lastInfo_["videoTechnologies"].value("fmp4", "");
        if (fmp4Url.empty())
            return "";

        int bestHeight = 0;
        std::string bestUrl;

        for (const auto &qual : lastInfo_["qualities"])
        {
            if (!qual.is_string())
                continue;

            std::string res = qual.get<std::string>();
            auto xPos = res.find('x');
            if (xPos == std::string::npos)
                continue;

            try
            {
                int height = std::stoi(res.substr(xPos + 1));
                if (height > bestHeight)
                {
                    bestHeight = height;
                    bestUrl = fmp4Url + "&variant=" + std::to_string(height);
                }
            }
            catch (...)
            {
                continue;
            }
        }

        return bestUrl;
    }

    Status AmateurTV::checkStatus()
    {
        std::string url = "https://www.amateur.tv/v3/readmodel/show/" + username() + "/en";

        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"},
            {"Referer", "https://amateur.tv/"}};

        HttpRequest req;
        req.url = url;
        req.timeoutSec = 30;
        req.headers = headers;

        auto resp = http().get(req);

        if (!resp.ok())
        {
            logger_->warn("HTTP {} for {}", resp.statusCode, username());
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::Error;
        }

        try
        {
            lastInfo_ = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);

            // Check for NOT_FOUND
            std::string message = lastInfo_.value("message", "");
            if (message == "NOT_FOUND")
                return Status::NotExist;

            // Check for KO result
            std::string result = lastInfo_.value("result", "");
            if (result == "KO")
                return Status::Error;

            // Check status
            std::string status = lastInfo_.value("status", "");
            // Lowercase it
            std::transform(status.begin(), status.end(), status.begin(), ::tolower);

            if (status == "online")
            {
                // privateChatStatus not null means private
                if (lastInfo_.contains("privateChatStatus") && !lastInfo_["privateChatStatus"].is_null())
                    return Status::Private;
                return Status::Public;
            }

            if (status == "offline")
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

    std::string AmateurTV::getVideoUrl()
    {
        std::string url = selectBestQuality();
        if (url.empty())
            return "";
        return url;
    }

} // namespace sm
