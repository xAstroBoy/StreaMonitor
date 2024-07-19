// ─────────────────────────────────────────────────────────────────
// Flirt4Free site plugin
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"
#include <unordered_map>

namespace sm
{

    class Flirt4Free : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "Flirt4Free";
        static constexpr const char *kSiteSlug = "F4F";

        explicit Flirt4Free(const std::string &username)
            : SitePlugin(kSiteName, kSiteSlug, username)
        {
            maxConsecutiveErrors_ = 100;
        }

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override
        {
            return "https://www.flirt4free.com/?model=" + username();
        }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"light_yellow", {}};
        }

    private:
        bool resolveRoomId();
        std::string roomId_;
        std::string hlsUrl_;
        bool lastResolveWasConnectionError_ = false; // Track if last resolve failed due to network

        // Class-level model cache (shared among instances)
        static std::mutex modelCacheMutex_;
        static std::unordered_map<std::string, std::string> modelCache_; // seo_name -> id
        static std::chrono::steady_clock::time_point lastCacheRefresh_;
    };

} // namespace sm
