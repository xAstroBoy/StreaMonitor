// ─────────────────────────────────────────────────────────────────
// StreaMate site plugin (aliases: pornhublive)
// manifest-server.naiadsystems.com JSON API
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"
#include <nlohmann/json.hpp>

namespace sm
{

    class StreaMate : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "StreaMate";
        static constexpr const char *kSiteSlug = "SM";

        explicit StreaMate(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"grey", {}};
        }

    private:
        std::string selectBestEncoding();

        nlohmann::json lastInfo_;
    };

} // namespace sm
