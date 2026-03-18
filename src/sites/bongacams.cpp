#include "sites/bongacams.h"

namespace sm
{

    REGISTER_SITE(BongaCams);

    BongaCams::BongaCams(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
    }

    std::string BongaCams::getWebsiteUrl() const
    {
        return "https://bongacams.com/" + username();
    }

    Status BongaCams::checkStatus()
    {
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/x-www-form-urlencoded"},
            {"Referer", "https://de.bongacams.net/" + username()},
            {"Accept", "application/json, text/javascript, */*; q=0.01"},
            {"X-Requested-With", "XMLHttpRequest"}};

        std::string body = "method=getRoomData&args%5B%5D=" + username() + "&args%5B%5D=false";

        HttpRequest req;
        req.url = "https://de.bongacams.net/tools/amf.php";
        req.method = "POST";
        req.body = body;
        req.contentType = "application/x-www-form-urlencoded";
        req.headers = headers;
        req.timeoutSec = 30;

        auto resp = http().execute(req);
        if (!resp.ok())
        {
            logger_->warn("HTTP {}", resp.statusCode);
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::Error;
        }

        try
        {
            auto json = nlohmann::json::parse(resp.body);
            lastInfo_ = json;
            setLastApiResponse(resp.body);

            std::string apiStatus = json.value("status", "");
            if (apiStatus == "error")
                return Status::NotExist;

            auto pd = json.value("performerData", nlohmann::json::object());
            std::string showType = pd.value("showType", "");

            // Check if performer username changed (redirect / case fix)
            // BongaCams CDN paths are case-sensitive — we must use the
            // canonical username from the API, not what the user typed.
            std::string perfUsername = pd.value("username", "");
            if (!perfUsername.empty() && perfUsername != username())
            {
                logger_->info("Username redirect: {} → {}", username(), perfUsername);
                setUsername(perfUsername);
            }

            if (showType == "private" || showType == "group")
                return Status::Private;

            auto ld = json.value("localData", nlohmann::json::object());
            if (!ld.contains("videoServerUrl"))
                return Status::Offline;

            // Verify playlist is accessible
            std::string videoServer = ld.value("videoServerUrl", "");
            if (!videoServer.empty())
            {
                if (videoServer.find("http") != 0)
                    videoServer = "https:" + videoServer;

                std::string playlistUrl = videoServer + "/hls/stream_" + username() + "/playlist.m3u8";
                auto playResp = http().get(playlistUrl, 10);
                if (!playResp.ok() || playResp.body.size() <= 25)
                    return Status::Offline;
            }

            return Status::Public;
        }
        catch (const std::exception &e)
        {
            logger_->error("Parse error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::Error;
        }
    }

    std::string BongaCams::getVideoUrl()
    {
        if (lastInfo_.empty())
            return "";

        auto ld = lastInfo_.value("localData", nlohmann::json::object());
        std::string videoServer = ld.value("videoServerUrl", "");
        if (videoServer.empty())
            return "";

        if (videoServer.find("http") != 0)
            videoServer = "https:" + videoServer;

        std::string masterUrl = videoServer + "/hls/stream_" + username() + "/playlist.m3u8";
        return selectResolution(masterUrl);
    }

} // namespace sm
