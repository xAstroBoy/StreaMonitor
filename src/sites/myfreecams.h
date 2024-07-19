// ─────────────────────────────────────────────────────────────────
// MyFreeCams site plugin
// HTML scraping, model_id math for HLS URL construction
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"
#include <map>

namespace sm
{

    class MyFreeCams : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "MyFreeCams";
        static constexpr const char *kSiteSlug = "MFC";

        explicit MyFreeCams(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"white", {}};
        }

    private:
        std::string extractAttribute(const std::string &html,
                                     const std::string &attrName) const;
        std::string buildPlaylistUrl() const;

        std::map<std::string, std::string> attrs_;
        std::string videoUrl_;
    };

} // namespace sm
