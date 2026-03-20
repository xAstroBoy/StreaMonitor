#pragma once
#include "core/site_plugin.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sm
{

    class StripChat : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "StripChat";
        static constexpr const char *kSiteSlug = "SC";

        explicit StripChat(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;
        std::string getPreviewUrl() const override;
        bool supportsBulkUpdate() const override { return true; }

        // API mobile hint — used ONLY as a trigger for cross-register
        // dual-recording.  Actual mobile detection comes from stream
        // resolution (portrait = h > w), never from this flag.
        bool apiMobileHint() const override { return apiMobileHint_; }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"green", {}};
        }

    protected:
        // Protected constructor for VR subclass
        StripChat(const std::string &siteName, const std::string &siteSlug,
                  const std::string &username);

        // Fetch master playlist with mouflon key extraction and URL rewriting
        // Returns the best variant URL with pkey/pdkey auth params, or empty
        std::string getPlaylistWithKeys();

        nlohmann::json lastInfo_;
        int64_t modelId_ = 0;
        std::string hlsStreamName_;
        bool apiMobileHint_ = false; // API-reported mobile (broadcaster device), NOT trusted for folders
        bool isVr_ = false;          // for VR subclass
        std::string vrSuffix_;       // VR stream suffix

        // Lazy mouflon init — called on first use, not in constructor
        static void ensureMouflonInit();

    private:
        // Mouflon initialization flag (class-level, done once)
        static bool mouflonInitialized_;
        static std::mutex mouflonInitMutex_;
    };

} // namespace sm
