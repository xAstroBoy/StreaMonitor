// ─────────────────────────────────────────────────────────────────
// SexChatHU site plugin
// Adult Performer Network — bulk_update, performer list caching
// ─────────────────────────────────────────────────────────────────
#pragma once
#include "core/site_plugin.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <optional>
#include <mutex>

namespace sm
{

    class SexChatHU : public SitePlugin
    {
    public:
        static constexpr const char *kSiteName = "SexChatHU";
        static constexpr const char *kSiteSlug = "SCHU";

        explicit SexChatHU(const std::string &username);

        Status checkStatus() override;
        std::string getVideoUrl() override;
        std::string getWebsiteUrl() const override;
        bool supportsBulkUpdate() const override { return true; }

        std::pair<std::string, std::vector<std::string>> getSiteColor() const override
        {
            return {"dark_grey", {}};
        }

    private:
        static std::optional<std::string> findRoomIdFromList(
            const std::string &username, HttpClient &http,
            std::shared_ptr<spdlog::logger> logger);

        struct CachedPerformerList
        {
            nlohmann::json data;
            std::chrono::steady_clock::time_point fetchTime;
            std::mutex mutex;
        };
        static CachedPerformerList &getCache();

        static nlohmann::json fetchPerformerList(HttpClient &http);

        nlohmann::json lastInfo_;
        std::string roomId_;
    };

} // namespace sm
