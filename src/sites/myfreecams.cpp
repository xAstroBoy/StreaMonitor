// ─────────────────────────────────────────────────────────────────
// MyFreeCams site plugin — HTML scraping + model_id math
// Port from Python: share page scraping, campreview data attrs
// ─────────────────────────────────────────────────────────────────

#include "sites/myfreecams.h"
#include <regex>
#include <cstdint>

namespace sm
{

    REGISTER_SITE(MyFreeCams);

    MyFreeCams::MyFreeCams(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
        sleepOnOffline_ = 10;
        sleepOnRateLimit_ = 60;
        maxConsecutiveErrors_ = 100;
    }

    std::string MyFreeCams::getWebsiteUrl() const
    {
        return "https://www.myfreecams.com/#" + username();
    }

    // Extract a data-attribute value from the campreview div
    std::string MyFreeCams::extractAttribute(const std::string &html,
                                             const std::string &attrName) const
    {
        // Search for data-cam-preview-{attrName}-value="..."
        std::string needle = "data-cam-preview-" + attrName + "-value=\"";
        auto pos = html.find(needle);
        if (pos == std::string::npos)
            return "";
        pos += needle.size();
        auto endPos = html.find('"', pos);
        if (endPos == std::string::npos)
            return "";
        return html.substr(pos, endPos - pos);
    }

    std::string MyFreeCams::buildPlaylistUrl() const
    {
        auto sidIt = attrs_.find("server-id");
        auto midIt = attrs_.find("model-id");
        auto wzobsIt = attrs_.find("is-wzobs");

        if (sidIt == attrs_.end() || midIt == attrs_.end())
            return "";

        std::string sid = sidIt->second;
        std::string midStr = midIt->second;

        if (sid.empty() || midStr.empty())
            return "";

        try
        {
            int64_t mid = 100000000 + std::stoll(midStr);
            std::string aPrefix = (wzobsIt != attrs_.end() && wzobsIt->second == "true") ? "a_" : "";

            return "https://previews.myfreecams.com/hls/NxServer/" + sid +
                   "/ngrp:mfc_" + aPrefix + std::to_string(mid) +
                   ".f4v_mobile_mhp1080_previewurl/playlist.m3u8";
        }
        catch (const std::exception &e)
        {
            return "";
        }
    }

    Status MyFreeCams::checkStatus()
    {
        // Step 1: Fetch the share page
        std::string shareUrl = "https://share.myfreecams.com/" + username();
        auto resp = http().get(shareUrl, 30);

        if (resp.isNotFound())
        {
            setLastError("Not found (404)", resp.statusCode);
            return Status::NotExist;
        }
        if (!resp.ok())
        {
            logger_->warn("HTTP {} for {}", resp.statusCode, username());
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::RateLimit;
        }

        const std::string &body = resp.body;
        setLastApiResponse(body.substr(0, 2048)); // Store first 2KB of HTML

        // Step 2: Look for tracking URL to verify model exists
        auto trackPos = body.find("https://www.myfreecams.com/php/tracking.php?");
        if (trackPos == std::string::npos)
            return Status::NotExist;

        // Verify model_id is in the tracking URL
        auto trackEnd = body.find('"', trackPos);
        if (trackEnd == std::string::npos)
            return Status::NotExist;

        std::string trackUrl = body.substr(trackPos, trackEnd - trackPos);
        if (trackUrl.find("model_id=") == std::string::npos)
            return Status::NotExist;

        // Step 3: Extract campreview data attributes
        auto camprevPos = body.find("class=\"campreview\"");
        if (camprevPos == std::string::npos)
        {
            // Also try alternative class name
            camprevPos = body.find("class='campreview'");
        }

        if (camprevPos == std::string::npos)
            return Status::Offline;

        // Extract a chunk around the campreview div
        size_t chunkStart = (camprevPos > 500) ? camprevPos - 500 : 0;
        size_t chunkEnd = std::min(camprevPos + 2000, body.size());
        std::string chunk = body.substr(chunkStart, chunkEnd - chunkStart);

        attrs_.clear();
        attrs_["server-id"] = extractAttribute(chunk, "server-id");
        attrs_["model-id"] = extractAttribute(chunk, "model-id");
        attrs_["is-wzobs"] = extractAttribute(chunk, "is-wzobs");

        if (attrs_["model-id"].empty())
            return Status::Offline;

        // Step 4: Build playlist URL and verify it
        std::string playlistUrl = buildPlaylistUrl();
        if (playlistUrl.empty())
            return Status::Offline;

        // Try to fetch the playlist to confirm it works
        auto playResp = http().get(playlistUrl, 30);
        if (playResp.ok())
        {
            videoUrl_ = selectResolution(playlistUrl);
            return videoUrl_.empty() ? Status::Private : Status::Public;
        }

        return Status::Private;
    }

    std::string MyFreeCams::getVideoUrl()
    {
        return videoUrl_;
    }

} // namespace sm
