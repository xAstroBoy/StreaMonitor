// ─────────────────────────────────────────────────────────────────
// DreamCamVR site plugin
// Inherits DreamCam, overrides to use video3D streamType
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "sites/dreamcam.h"
#include <map>

namespace sm
{

    class DreamCamVR : public DreamCam
    {
    public:
        static constexpr const char *kSiteName = "DreamCamVR";
        static constexpr const char *kSiteSlug = "DCVR";

        explicit DreamCamVR(const std::string &username);

        // VR streams are NEVER mobile
        bool isMobile() const override { return false; }

        std::string getWebsiteUrl() const override
        {
            return "https://dreamcamtrue.com/" + username();
        }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"light_cyan", {"bold"}};
        }

    private:
        static const std::map<std::string, std::string> kVrFrameFormatMap;
    };

} // namespace sm
