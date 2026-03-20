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

    std::string AmateurTV::selectBestQuality()
    {
        // Port from Python: getPlaylistVariants() override + getWantedResolutionPlaylist(None)
        // Parse qualities array (e.g. ["640x480", "1280x720", "1920x1080"])
        // Build variant entries and apply the user's resolution preference
        if (!lastInfo_.contains("qualities") || !lastInfo_["qualities"].is_array())
            return "";
        if (!lastInfo_.contains("videoTechnologies") || !lastInfo_["videoTechnologies"].is_object())
            return "";

        std::string fmp4Url = lastInfo_["videoTechnologies"].value("fmp4", "");
        if (fmp4Url.empty())
            return "";

        // Build variant list from API qualities (like Python's getPlaylistVariants)
        struct QualityVariant
        {
            int width;
            int height;
            std::string url;
            int diff; // min(w,h) - wantedResolution
        };
        std::vector<QualityVariant> variants;

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
                int width = std::stoi(res.substr(0, xPos));
                int height = std::stoi(res.substr(xPos + 1));
                std::string url = fmp4Url + "&variant=" + std::to_string(height);
                variants.push_back({width, height, url, 0});
            }
            catch (...)
            {
                continue;
            }
        }

        if (variants.empty())
            return "";

        // Apply user's resolution preference (mirrors Python getWantedResolutionPlaylist)
        int wantedRes = config_ ? config_->wantedResolution : 99999;
        ResolutionPref pref = config_ ? config_->resolutionPref : ResolutionPref::Closest;

        for (auto &v : variants)
        {
            int minDim = std::min(v.width, v.height);
            if (minDim == 0)
                minDim = v.height;
            v.diff = minDim - wantedRes;
        }

        std::sort(variants.begin(), variants.end(),
                  [](const QualityVariant &a, const QualityVariant &b)
                  { return std::abs(a.diff) < std::abs(b.diff); });

        const QualityVariant *selected = nullptr;

        switch (pref)
        {
        case ResolutionPref::Exact:
            if (variants[0].diff == 0)
                selected = &variants[0];
            break;
        case ResolutionPref::Closest:
            selected = &variants[0];
            break;
        case ResolutionPref::ExactOrLeastHigher:
            for (const auto &v : variants)
            {
                if (v.diff >= 0)
                {
                    selected = &v;
                    break;
                }
            }
            break;
        case ResolutionPref::ExactOrHighestLower:
            for (const auto &v : variants)
            {
                if (v.diff <= 0)
                {
                    selected = &v;
                    break;
                }
            }
            break;
        }

        if (!selected)
        {
            logger_->error("Couldn't select a resolution from qualities");
            return "";
        }

        logger_->info("Selected quality: {}x{}", selected->width, selected->height);
        setRecordingResolution(selected->width, selected->height);
        return selected->url;
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

            // Canonical username from API — update if different
            std::string apiUsername = lastInfo_.value("username", "");
            if (!apiUsername.empty() && apiUsername != username())
            {
                logger_->info("Username case fix: {} → {}", username(), apiUsername);
                setUsername(apiUsername);
            }

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
        setMasterUrl(url);
        return url;
    }

} // namespace sm
