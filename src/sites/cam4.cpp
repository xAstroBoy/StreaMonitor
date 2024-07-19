// ─────────────────────────────────────────────────────────────────
// Cam4 site plugin — 3-step REST API (Python-faithful)
// Step 1: GET /rest/v1.0/profile/{user}/info → check online
// Step 2: GET webchat.cam4.com/requestAccess?roomname={user} → private
// Step 3: GET /rest/v1.0/profile/{user}/streamInfo → cdnURL
// ─────────────────────────────────────────────────────────────────

#include "sites/cam4.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace sm
{

    REGISTER_SITE(Cam4);

    Status Cam4::checkStatus()
    {
        // Python Step 1: profile/{user}/info (NOT profile/{user})
        HttpRequest req;
        req.url = "https://hu.cam4.com/rest/v1.0/profile/" + username() + "/info";
        req.headers["Accept"] = "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8";

        auto resp = http().get(req);
        if (resp.statusCode == 403)
        {
            setLastError("Restricted (403)", resp.statusCode);
            return Status::Restricted;
        }
        if (resp.statusCode == 404)
        {
            setLastError("Not found (404)", resp.statusCode);
            return Status::NotExist;
        }
        if (resp.statusCode != 200)
        {
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::RateLimit;
        }

        // Python: check profile.online field
        try
        {
            auto pj = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);
            bool online = pj.value("online", false);
            if (!online)
                return Status::Offline;
        }
        catch (...)
        {
            setLastError("JSON parse error (profile)", resp.statusCode);
            return Status::Error;
        }

        // Python Step 2: webchat.cam4.com/requestAccess (NOT profile/{user}/room)
        HttpRequest roomReq;
        roomReq.url = "https://webchat.cam4.com/requestAccess?roomname=" + username();
        roomReq.headers["Accept"] = "application/json";

        auto roomResp = http().get(roomReq);
        if (roomResp.statusCode != 200)
        {
            setLastError("Room access HTTP " + std::to_string(roomResp.statusCode), roomResp.statusCode);
            return Status::Offline;
        }

        try
        {
            auto rj = nlohmann::json::parse(roomResp.body);
            if (rj.value("privateStream", false))
                return Status::Private;
        }
        catch (...)
        {
        }

        // Python Step 3: streamInfo → cdnURL (NOT hlsPreviewUrl)
        HttpRequest streamReq;
        streamReq.url = "https://hu.cam4.com/rest/v1.0/profile/" + username() + "/streamInfo";
        streamReq.headers["Accept"] = "application/json";

        auto streamResp = http().get(streamReq);
        if (streamResp.statusCode == 204)
            return Status::Offline;
        if (streamResp.statusCode != 200)
        {
            setLastError("StreamInfo HTTP " + std::to_string(streamResp.statusCode), streamResp.statusCode);
            return Status::RateLimit;
        }

        try
        {
            auto sj = nlohmann::json::parse(streamResp.body);
            setLastApiResponse(streamResp.body);
            // Python field: cdnURL (NOT hlsPreviewUrl)
            if (sj.contains("cdnURL"))
            {
                hlsUrl_ = sj["cdnURL"].get<std::string>();
                return Status::Public;
            }
        }
        catch (const std::exception &e)
        {
            logger_->warn("Cam4 JSON parse error: {}", e.what());
        }

        return Status::Offline;
    }

    std::string Cam4::getVideoUrl()
    {
        if (hlsUrl_.empty())
            return {};
        return selectResolution(hlsUrl_);
    }

} // namespace sm
