// ─────────────────────────────────────────────────────────────────
// XLoveCam site plugin
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"

namespace sm
{

    class XLoveCam : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "XLoveCam";
        static constexpr const char *kSiteSlug = "XLC";

        explicit XLoveCam(const std::string &username)
            : SitePlugin(kSiteName, kSiteSlug, username)
        {
            maxConsecutiveErrors_ = 100;
        }

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override
        {
            return "https://www.xlovecam.com/" + username();
        }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"light_white", {}};
        }

    private:
        bool resolvePerformerId();
        std::string performerId_;
        std::string hlsUrl_;
    };

} // namespace sm
