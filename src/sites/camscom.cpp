// ─────────────────────────────────────────────────────────────────
// Cams.com site plugin — beta-api + hardcoded CDN
// Port from Python: CCModelInfo, online code mapping
// Online flags: 0=Off, 1=Public, 2=Nude, 3=Private, 4=Admin,
//   6=Ticket, 7=Voyeur, 10=Party, 11=GoalUp, 12=GoalDown,
//   13=Group, 14=C2C
// ─────────────────────────────────────────────────────────────────

#include "sites/camscom.h"
#include <algorithm>

namespace sm
{

    REGISTER_SITE(CamsCom);

    // Public show codes: 1, 2, 6, 10, 11, 12
    const std::set<int> CamsCom::kPublicCodes = {1, 2, 6, 10, 11, 12};
    // Private show codes: 3, 4, 7, 13, 14
    const std::set<int> CamsCom::kPrivateCodes = {3, 4, 7, 13, 14};

    CamsCom::CamsCom(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
        sleepOnOffline_ = 10;
        sleepOnRateLimit_ = 60;
        maxConsecutiveErrors_ = 100;
    }

    std::string CamsCom::getWebsiteUrl() const
    {
        return "https://cams.com/" + username();
    }

    Status CamsCom::mapOnlineCode(int code, const std::string &streamName) const
    {
        if (streamName.empty())
            return Status::NotExist;
        if (code == 0)
            return Status::Offline;
        if (kPublicCodes.count(code))
            return Status::Public;
        if (kPrivateCodes.count(code))
            return Status::Private;
        // Unknown code — treat as private
        if (code > 0)
            return Status::Private;
        return Status::Unknown;
    }

    Status CamsCom::checkStatus()
    {
        std::string url = "https://beta-api.cams.com/models/stream/" + username() + "/";
        auto resp = http().get(url, 30);

        if (!resp.ok())
        {
            logger_->warn("HTTP {} for {}", resp.statusCode, username());
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::Error;
        }

        try
        {
            lastInfo_ = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);

            std::string streamName = lastInfo_.value("stream_name", "");
            int online = -1;

            if (lastInfo_.contains("online"))
            {
                auto &onlineVal = lastInfo_["online"];
                if (onlineVal.is_number())
                    online = onlineVal.get<int>();
                else if (onlineVal.is_string())
                {
                    try
                    {
                        online = std::stoi(onlineVal.get<std::string>());
                    }
                    catch (...)
                    {
                        online = -1;
                    }
                }
            }

            return mapOnlineCode(online, streamName);
        }
        catch (const std::exception &e)
        {
            logger_->error("Parse error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::Error;
        }
    }

    std::string CamsCom::getVideoUrl()
    {
        // Hardcoded CDN URL pattern — lowercased username
        std::string lowerUser = username();
        std::transform(lowerUser.begin(), lowerUser.end(), lowerUser.begin(), ::tolower);
        std::string url = "https://camscdn.cams.com/camscdn/cdn-" + lowerUser + ".m3u8";
        return selectResolution(url);
    }

} // namespace sm
