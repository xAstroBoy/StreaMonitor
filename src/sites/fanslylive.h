// ─────────────────────────────────────────────────────────────────
// FanslyLive site plugin
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"

namespace sm
{

    class FanslyLive : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "FanslyLive";
        static constexpr const char *kSiteSlug = "FL";

        explicit FanslyLive(const std::string &username)
            : SitePlugin(kSiteName, kSiteSlug, username)
        {
            maxConsecutiveErrors_ = 100;
        }

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override
        {
            return "https://fansly.com/live/" + username();
        }

    private:
        bool resolveRoomId();
        std::string roomId_;
        std::string hlsUrl_;
    };

} // namespace sm
