// ─────────────────────────────────────────────────────────────────
// DreamCam site plugin
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"

namespace sm
{

    class DreamCam : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "DreamCam";
        static constexpr const char *kSiteSlug = "DC";

        explicit DreamCam(const std::string &username)
            : SitePlugin(kSiteName, kSiteSlug, username)
        {
            maxConsecutiveErrors_ = 80;
        }

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override
        {
            return "https://dreamcamtrue.com/" + username();
        }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"light_blue", {}};
        }

    protected:
        // Protected constructor for VR subclass
        DreamCam(const std::string &siteName, const std::string &siteSlug,
                 const std::string &username)
            : SitePlugin(siteName, siteSlug, username)
        {
            maxConsecutiveErrors_ = 80;
        }

        std::string streamUrl_;
        std::string streamType_ = "video2D"; // VR subclass overrides to "video3D"
    };

} // namespace sm
