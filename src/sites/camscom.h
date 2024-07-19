// ─────────────────────────────────────────────────────────────────
// Cams.com site plugin
// beta-api.cams.com REST + hardcoded CDN URL pattern
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"
#include <nlohmann/json.hpp>
#include <set>

namespace sm
{

    class CamsCom : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "CamsCom";
        static constexpr const char *kSiteSlug = "CC";

        explicit CamsCom(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"light_red", {}};
        }

    private:
        // Online status codes
        static const std::set<int> kPublicCodes;
        static const std::set<int> kPrivateCodes;

        Status mapOnlineCode(int code, const std::string &streamName) const;

        nlohmann::json lastInfo_;
    };

} // namespace sm
