#pragma once
#include "core/site_plugin.h"
#include <nlohmann/json.hpp>

namespace sm
{

    class CamSoda : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "CamSoda";
        static constexpr const char *kSiteSlug = "CS";

        explicit CamSoda(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;
        std::string getPreviewUrl() const override
        {
            return "https://md.camsoda.com/thumbs/" + username() + ".jpg";
        }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"blue", {}};
        }

    private:
        nlohmann::json lastInfo_;
        static constexpr const char *kApiBase = "https://www.camsoda.com/api/v1/chat/react";
    };

} // namespace sm
