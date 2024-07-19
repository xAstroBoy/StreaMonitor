#pragma once
#include "core/site_plugin.h"
#include <nlohmann/json.hpp>

namespace sm
{

    class BongaCams : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "BongaCams";
        static constexpr const char *kSiteSlug = "BC";

        explicit BongaCams(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;
        std::string getPreviewUrl() const override
        {
            return "https://ri.bongacams.com/stream_snapshot/" + username() + ".jpg";
        }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"yellow", {}};
        }

    private:
        nlohmann::json lastInfo_;
    };

} // namespace sm
