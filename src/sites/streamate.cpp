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

    std::string StreaMate::selectBestEncoding()
    {
        // Port from Python: getPlaylistVariants() override + getWantedResolutionPlaylist(None)
        // Extract encodings from formats.mp4-hls.encodings array
        // Apply user's resolution preference (not just highest!)
        if (!lastInfo_.contains("formats"))
            return "";

        auto &formats = lastInfo_["formats"];
        if (!formats.contains("mp4-hls"))
            return "";

        auto &mp4hls = formats["mp4-hls"];
        if (!mp4hls.contains("encodings") || !mp4hls["encodings"].is_array())
            return "";

        // Build variant list from API encodings (like Python's getPlaylistVariants)
        struct EncodingVariant
        {
            int width;
            int height;
            std::string url;
            int diff; // min(w,h) - wantedResolution
        };
        std::vector<EncodingVariant> variants;

        for (const auto &enc : mp4hls["encodings"])
        {
            std::string location = enc.value("location", "");
            int width = enc.value("videoWidth", 0);
            int height = enc.value("videoHeight", 0);
            if (!location.empty() && height > 0)
                variants.push_back({width, height, location, 0});
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
                  [](const EncodingVariant &a, const EncodingVariant &b)
                  { return std::abs(a.diff) < std::abs(b.diff); });

        const EncodingVariant *selected = nullptr;

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
            logger_->error("Couldn't select encoding resolution");
            return "";
        }

        logger_->info("Selected encoding: {}x{}", selected->width, selected->height);
        return selected->url;
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
