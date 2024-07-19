// ─────────────────────────────────────────────────────────────────
// ManyVids site plugin
// Roompool API + CloudFront cookies + player-settings
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"
#include <nlohmann/json.hpp>

namespace sm
{

    class ManyVids : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "ManyVids";
        static constexpr const char *kSiteSlug = "MV";

        explicit ManyVids(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"light_green", {}};
        }

    private:
        std::string requestStreamInfo();
        void updateSiteCookies();
        std::string extractCloudFrontUrl(const std::string &policyCookie) const;

        nlohmann::json lastInfo_;
        std::string cloudFrontCookies_; // Raw cookie header for media requests
    };

} // namespace sm
