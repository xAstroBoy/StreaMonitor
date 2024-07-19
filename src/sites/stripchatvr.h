// ─────────────────────────────────────────────────────────────────
// StripChatVR site plugin
// Inherits StripChat, adds VR capability detection + format suffix
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "sites/stripchat.h"
#include <map>

namespace sm
{

    class StripChatVR : public StripChat
    {
    public:
        static constexpr const char *kSiteName = "StripChatVR";
        static constexpr const char *kSiteSlug = "SCVR";

        explicit StripChatVR(const std::string &username);

        Status checkStatus() override;
        std::string getWebsiteUrl() const override;
        bool supportsBulkUpdate() const override { return false; }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"cyan", {"bold"}};
        }

    private:
        // VR capability detection
        bool isVrCapable() const;
        bool getIsVrModel() const;
        bool getHasVrSettings() const;

        // JSON path helpers
        nlohmann::json findInPaths(const nlohmann::json &root,
                                   const std::vector<std::vector<std::string>> &paths) const;
        nlohmann::json recursiveFind(const nlohmann::json &root,
                                     const std::string &key) const;
        nlohmann::json findVrCamSettings() const;

        // VR format suffix
        std::string getVrSuffix() const;

        static const std::map<std::string, std::string> kVrFrameFormatMap;

        bool lastVrCapableState_ = false;
    };

} // namespace sm
