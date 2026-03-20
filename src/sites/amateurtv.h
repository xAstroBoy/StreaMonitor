// ─────────────────────────────────────────────────────────────────
// AmateurTV site plugin
// GET /v3/readmodel/show/{username}/en — fmp4 video technologies
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"
#include <nlohmann/json.hpp>

namespace sm
{

    class AmateurTV : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "AmateurTV";
        static constexpr const char *kSiteSlug = "ATV";

        explicit AmateurTV(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"light_grey", {}};
        }

    private:
        std::string selectBestQuality();

        nlohmann::json lastInfo_;
    };

} // namespace sm
