// ─────────────────────────────────────────────────────────────────
// Cam4 site plugin
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"

namespace sm
{

    class Cam4 : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "Cam4";
        static constexpr const char *kSiteSlug = "C4";

        explicit Cam4(const std::string &username)
            : SitePlugin(kSiteName, kSiteSlug, username)
        {
            maxConsecutiveErrors_ = 100;
        }

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override
        {
            return "https://hu.cam4.com/" + username();
        }
        std::string getPreviewUrl() const override
        {
            return "https://snapshots.cam4.com/thumbnail/" + username() + "_600.jpg";
        }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"red", {}};
        }

    private:
        std::string hlsUrl_;
    };

} // namespace sm
