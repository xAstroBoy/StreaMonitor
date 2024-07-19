#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Chaturbate site plugin
// ─────────────────────────────────────────────────────────────────

#include "core/site_plugin.h"
#include <nlohmann/json.hpp>

namespace sm
{

    class Chaturbate : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "Chaturbate";
        static constexpr const char *kSiteSlug = "CB";

        explicit Chaturbate(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;
        std::string getPreviewUrl() const override
        {
            return "https://roomimg.stream.highwebmedia.com/ri/" + username() + ".jpg";
        }
        bool supportsBulkUpdate() const override { return true; }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"magenta", {}};
        }

    private:
        Status parseRoomStatus(const std::string &roomStatus, bool hasUrl) const;
        std::string buildCmafUrl(const std::string &url) const;

        nlohmann::json lastInfo_;
        std::vector<std::string> backupEndpoints_;
        int requestFailureCount_ = 0;
    };

} // namespace sm
