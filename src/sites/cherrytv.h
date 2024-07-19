// ─────────────────────────────────────────────────────────────────
// Cherry.tv site plugin
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"

namespace sm
{

    class CherryTV : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "CherryTV";
        static constexpr const char *kSiteSlug = "CHTV";

        explicit CherryTV(const std::string &username)
            : SitePlugin(kSiteName, kSiteSlug, username)
        {
            maxConsecutiveErrors_ = 100;
        }

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override
        {
            return "https://www.cherry.tv/" + username();
        }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"light_magenta", {}};
        }

    private:
        std::string hlsUrl_;
    };

} // namespace sm
